/*
 * fast_life_esp32.ino — Fast Game of Life for ESP32 with U8g2, NeoPixel & SD Logging
 *
 * Features:
 *  - 1-Pixel Mode (128x64) and 2-Pixel Mode (64x32)
 *  - Startup Menu (select mode)
 *  - Optimized 64-bit column engine
 *  - SD Card Logging (optional, non-blocking)
 *  - Dynamic LED Feedback (Status & Population Density)
 *
 * Hardware:
 *   - ESP32
 *   - OLED: SH1106/SSD1306 (I2C 0x3C)
 *   - Buttons: Pin 2 (A), Pin 1 (B) [Swapped per user req]
 *   - NeoPixel: Pin 48 (1 LED)
 *   - SD Card: Custom HSPI Pins (CS=7, MOSI=6, MISO=4, CLK=5)
 *
 * Libraries:
 *   - U8g2
 *   - Adafruit_NeoPixel
 *   - SD, FS, SPI
 *
 * Author: Alejandro Rebolledo (arebolledo@udd.cl)
 * Date: 2025-12-19
 * License: CC BY-NC 4.0
 */

#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_NeoPixel.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"

// ============================================================================
// CONSTANTS
// ============================================================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define I2C_ADDR 0x3C

// Loop detection
#define SHORT_HASH_INTERVAL 6
#define LONG_HASH_INTERVAL 256
#define LONG_HASH_RESET_PROB 0.01f 

#define FRAME_TIME 20 // Target frame time in ms
#define MENU_TIMEOUT_MS 10000

// Pins
static constexpr int BTNA_PIN = 2; // Swapped
static constexpr int BTNB_PIN = 1; // Swapped
static constexpr int RGB_LED_PIN = 48;
static constexpr int DEBOUNCE_MS = 60;

// Custom SD Pins
static constexpr int SD_CS_PIN   = 7;
static constexpr int SD_MOSI_PIN = 6;
static constexpr int SD_MISO_PIN = 4;
static constexpr int SD_SCLK_PIN = 5;

// LED Config
#define NUM_RGB_LEDS 1

// ============================================================================
// GLOBALS
// ============================================================================
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
Adafruit_NeoPixel rgbLed(NUM_RGB_LEDS, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);
SPIClass spiSD(HSPI);

// SD State
bool sdOK = false;

// ENGINE STATE
// Max width 128 (for scale 1). Each column is uint64_t (max height 64).
uint64_t board[128];
uint64_t first_col;
uint64_t this_col;
uint64_t last_col;

// Hash State
uint32_t short_check_hash = 0;
uint32_t long_check_hash = 0;
uint32_t generation = 0;
uint32_t start_t = 0;
uint32_t frame_start_t = 0;

// Stats
uint32_t population = 0;

// Config State
int currentScale = 2; // Default 2
int boardWidth = 64;
int boardHeight = 32;

// Buttons
volatile bool btnA_pressed = false;
volatile bool btnB_pressed = false;
volatile uint32_t last_btnA_time = 0;
volatile uint32_t last_btnB_time = 0;

// Bit count LUT (for 3 bit chunk 0..7)
static const uint8_t bit_counts[] = { 0, 1, 1, 2, 1, 2, 2, 3 };

// ============================================================================
// HELPERS
// ============================================================================

void setLedStatus(uint8_t r, uint8_t g, uint8_t b) {
    rgbLed.setPixelColor(0, rgbLed.Color(r, g, b));
    rgbLed.show();
}

void logHashMatch(const char* type, uint32_t hash, uint32_t gen) {
    if (!sdOK) return;
    File f = SD.open("/life_log.txt", FILE_APPEND);
    if (f) {
        f.printf("[%lu] %s Match: 0x%08X at Gen %u\n", millis(), type, hash, gen);
        f.close();
    }
}

// Rotate 64-bit column 'x' by 'n' positions (up/down shift with wrap)
static inline uint32_t rotate_shift_three_64(uint64_t x, int n) {
    int r = (n == 0) ? 63 : (n - 1);
    uint64_t rot = (x >> r) | (x << (64 - r));
    return (uint32_t)(rot & 0x7);
}

// 32-bit version for Scale 2
static inline uint32_t rotate_shift_three_32(uint64_t x, int n) {
    int r = (n == 0) ? 31 : (n - 1);
    uint32_t x32 = (uint32_t)x;
    uint32_t rot = (x32 >> r) | (x32 << (32 - r));
    return rot & 0x7;
}

void drawCell(int x, int y, int c) {
    if (!c) return;
    if (currentScale == 2) {
        u8g2.drawPixel(x*2, y*2);
        u8g2.drawPixel(x*2+1, y*2);
        u8g2.drawPixel(x*2, y*2+1);
        u8g2.drawPixel(x*2+1, y*2+1);
    } else {
        u8g2.drawPixel(x, y);
    }
}

// Draw entire board from memory
void drawFullBoard() {
    u8g2.clearBuffer();
    for (int x = 0; x < boardWidth; x++) {
        uint64_t col = board[x];
        for (int y = 0; y < boardHeight; y++) {
            if (col & 1) drawCell(x, y, 1);
            col >>= 1;
        }
    }
    u8g2.sendBuffer();
}

void randomizeBoard() {
    for (int x = 0; x < boardWidth; x++) {
        uint64_t r1 = ((uint64_t)esp_random() << 32) | esp_random();
        uint64_t r2 = ((uint64_t)esp_random() << 32) | esp_random();
        // 25% density
        board[x] = r1 & r2;
        
        if (currentScale == 2) {
            board[x] &= 0xFFFFFFFF;
        }
    }
}

void resetGame(uint32_t hash, uint32_t duration_us) {
    // Flash White on Reset
    setLedStatus(255, 255, 255);
    
    if (hash != 0) {
        Serial.printf("RESET: Hash=0x%08X Gen=%u\n", hash, generation);
        logHashMatch("Reset", hash, generation);
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.setCursor(10, 20);
    u8g2.printf("Gen: %u", generation);
    u8g2.setCursor(10, 40);
    u8g2.printf("Time: %.1fs", duration_us / 1000000.0);
    u8g2.sendBuffer();
    delay(1500);
    
    randomizeBoard();
    drawFullBoard();
    
    start_t = micros();
    generation = 0;
    short_check_hash = 0;
    long_check_hash = 0;
    
    // Return to status color (Green/Orange) briefly before evolving
    if(sdOK) setLedStatus(0, 50, 0); else setLedStatus(50, 20, 0); 
}

// ============================================================================
// INTERRUPTS
// ============================================================================
void IRAM_ATTR isrBtnA() {
    uint32_t now = millis();
    if (now - last_btnA_time > DEBOUNCE_MS) {
        btnA_pressed = true;
        last_btnA_time = now;
    }
}
void IRAM_ATTR isrBtnB() {
    uint32_t now = millis();
    if (now - last_btnB_time > DEBOUNCE_MS) {
        btnB_pressed = true;
        last_btnB_time = now;
    }
}

// ============================================================================
// EVOLUTION ENGINE
// ============================================================================
void evolve() {
    u8g2.clearBuffer();
    
    uint32_t hash = 5381;
    population = 0;
    
    for (int x = 0; x < boardWidth; x++) {
        uint64_t left  = (x == 0) ? board[boardWidth - 1] : board[x - 1];
        uint64_t center = board[x];
        uint64_t right = (x == boardWidth - 1) ? board[0] : board[x + 1];
        
        this_col = 0;
        uint32_t hash_acc = 0;
        
        for (int y = 0; y < boardHeight; y++) {
            hash_acc <<= 1;
            
            uint32_t l, c, r;
            if (currentScale == 2) {
                l = rotate_shift_three_32(left, y);
                c = rotate_shift_three_32(center, y);
                r = rotate_shift_three_32(right, y);
            } else {
                l = rotate_shift_three_64(left, y);
                c = rotate_shift_three_64(center, y);
                r = rotate_shift_three_64(right, y);
            }
            
            int count = bit_counts[l] + bit_counts[c] + bit_counts[r];
            bool alive = (c & 0x2);
            
            if ((count == 3) || (count == 4 && alive)) {
                this_col |= ((uint64_t)1 << y);
                hash_acc |= 1;
                population++;
            }
            
            if ((y & 0x7) == 0x7) {
                hash = ((hash << 5) + hash) ^ hash_acc;
                hash_acc = 0;
            }
        } // y
        
        if (x > 0) {
            board[x - 1] = last_col;
            uint64_t dc = last_col;
            for(int y=0; y<boardHeight; y++) {
                if(dc & 1) drawCell(x-1, y, 1);
                dc >>= 1;
            }
        } else {
            first_col = this_col;
        }
        last_col = this_col;
    } // x
    
    board[boardWidth - 1] = last_col;
    board[0] = first_col;
    
    uint64_t dc = first_col;
    for(int y=0; y<boardHeight; y++) { if(dc & 1) drawCell(0, y, 1); dc >>= 1; }
    dc = last_col;
    for(int y=0; y<boardHeight; y++) { if(dc & 1) drawCell(boardWidth-1, y, 1); dc >>= 1; }
    
    u8g2.sendBuffer();
    
    // VISUAL FEEDBACK: Population Density
    // Total cells: Scale1=8192, Scale2=2048
    // Game of Life usually hovers around 3-10% density? Or more?
    // Let's map density simply. 
    // < 500 (Scale2): Blue-ish. > 500: Red-ish.
    // Normalized p: 0.0 to 1.0 (approximated)
    
    float total = (float)(boardWidth * boardHeight);
    float density = (float)population / total;
    
    // Color Map: Low(0.1)=Blue, Med(0.2)=Purple, High(>0.3)=Red
    // Scale density to 0-255 range for logic
    // Usually density stabilizes ~0.37? No, GoL is sparse. often 0.03 to 0.10.
    // Let's amplify. Map 0.0 -> 0.4 range to Blue->Red. chagned intesity from 255 to 155 led too brigh
    
    float intensity = density * 3.0f; // 0.33 density -> 1.0
    if (intensity > 1.0f) intensity = 1.0f;
    
    uint8_t r = (uint8_t)(intensity * 155);
    uint8_t b = (uint8_t)((1.0f - intensity) * 155);
    // Add pulsing Green component based on generation for "Heartbeat"
    uint8_t g = (generation % 10) * 10; 
    
    setLedStatus(r, g, b);

    // Hash Checks
    if (short_check_hash == hash) {
        Serial.println("Short Loop Detected");
        logHashMatch("ShortLoop", hash, generation);
        resetGame(hash, micros() - start_t);
    } else if (long_check_hash == hash) {
        // User requested to allow infinite looping.
        // We log the event but do NOT reset.
        Serial.println("Long Loop Detected - Continuing...");
        logHashMatch("LongLoop", hash, generation);
        
        // Advance generation so we don't get stuck logic-wise (though state is looping)
        generation++;
    } else {
        if (generation % SHORT_HASH_INTERVAL == 0) short_check_hash = hash;
        if (generation % LONG_HASH_INTERVAL == 0) long_check_hash = hash;
        generation++;
    }
}


// ============================================================================
// SETUP
// ============================================================================
void setup() {
    Serial.begin(115200);
    rgbLed.begin(); 
    rgbLed.clear(); 
    rgbLed.show();
    
    pinMode(BTNA_PIN, INPUT_PULLUP);
    attachInterrupt(BTNA_PIN, isrBtnA, FALLING);
    pinMode(BTNB_PIN, INPUT_PULLUP);
    attachInterrupt(BTNB_PIN, isrBtnB, FALLING);
    
    u8g2.begin();
    
    // --- SD INIT (Custom HSPI) ---
    spiSD.begin(SD_SCLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    if (SD.begin(SD_CS_PIN, spiSD)) {
        sdOK = true;
        Serial.println("SD Card OK");
        setLedStatus(0, 200, 0); // Green = OK
    } else {
        Serial.println("SD Card Failed");
        sdOK = false;
        setLedStatus(200, 100, 0); // Orange = Warning
    }

    // --- MENU ---
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(0, 10, sdOK ? "SD: OK" : "SD: No");
    u8g2.drawStr(0, 25, "Select Mode:");
    u8g2.drawStr(0, 40, "A: Scale 1 (128x64)");
    u8g2.drawStr(0, 55, "B: Scale 2 (64x32)");
    u8g2.sendBuffer();
    
    uint32_t t0 = millis();
    bool selected = false;
    
    while (millis() - t0 < MENU_TIMEOUT_MS) {
        float p = 1.0 - (float)(millis() - t0) / MENU_TIMEOUT_MS;
        u8g2.drawHLine(0, 63, (int)(128 * p));
        u8g2.sendBuffer();
        
        if (btnA_pressed) {
            currentScale = 1;
            selected = true;
            btnA_pressed = false;
            break;
        }
        if (btnB_pressed) {
            currentScale = 2;
            selected = true;
            btnB_pressed = false;
            break;
        }
        delay(10);
    }
    
    if (!selected) {
        currentScale = 2; // Default
    }
    
    if (currentScale == 1) {
        boardWidth = 128;
        boardHeight = 64;
    } else {
        boardWidth = 64;
        boardHeight = 32;
    }
    
    randomizeBoard();
    drawFullBoard();
    
    start_t = micros();
    frame_start_t = millis();
}

void loop() {
    if (btnA_pressed) {
        btnA_pressed = false;
        resetGame(0, micros() - start_t);
    }
    if (btnB_pressed) {
        btnB_pressed = false;
        u8g2.setPowerSave(1);
        esp_sleep_enable_ext0_wakeup((gpio_num_t)BTNA_PIN, 0);
        esp_deep_sleep_start();
    }
    
    evolve();
    
    while(millis() - frame_start_t < FRAME_TIME) yield();
    frame_start_t = millis();
}

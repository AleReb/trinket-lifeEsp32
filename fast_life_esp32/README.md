# Fast Life ESP32

A high-performance "Conway's Game of Life" implementation for **ESP32** with **OLED**, **NeoPixel**, **Buttons**, and **SD Card Logging**.

Ported and enhanced from the original Trinket M0 "Fast Life" implementation.

## Features

- **High Resolution Mode**: 128x64 1-pixel resolution (requires ESP32 power!).
- **Fast Mode**: 64x32 2-pixel resolution (classic chunky look).
- **Startup Menu**: Select your mode on boot (10s timeout defaults to Fast Mode).
- **Loop Detection**: Automatically detects stable states (Period 6) and long loops (Gliders).
- **SD Logging**: If an SD card is present, logs loop detection events to `/life_log.txt`.
- **Non-blocking**: Runs smoothly even if SD card is missing.

## Credits & Attribution

- **Author**: Alejandro Rebolledo (arebolledo@udd.cl)
- **License**: Creative Commons Attribution-NonCommercial 4.0 International (CC BY-NC 4.0).
- **Original Concept**: Based on "Tiny Life" by Eli Baum. [Review Original Project](https://elibaum.com/2021/03/25/pandemic-projects-tiny-life.html).

### How it Works (Loop Detection)

The core engine includes a pedagogical "End-Game Detection System" inspired by the original research:

> While glider infinite loops were quite rare, they ended up appearing enough to be annoying. I decided to implement a basic check for these cases: in addition to checking the hash every 6 generations, I would also check the hash every 256 generations.

**Why 256?** It takes exactly **256 generations** for a standard Glider to return to its original position on the 64-width board (wrapping around).

- **Short Loops (Period 6)**: Stable blocks or blinkers. These trigger an immediate reset.
- **Long Loops (Period 256)**: Gliders or complex spaceships. **These are logged but NOT reset**, allowing you to watch the infinite loop play out forever (as requested).
- **Dynamic LED**: The NeoPixel changes color based on **Population Density** (Blue=Sparse -> Red=Dense) and glows with simulation speed.

## Hardware Configuration

| Component | ESP32 Pin | Logic |
| :--- | :--- | :--- |
| **Button A** | GPIO **2** | Reset Game (Menu: Select Scale 1) |
| **Button B** | GPIO **1** | Sleep / Menu Select Scale 2 |
| **OLED SDA** | GPIO **21** | I2C Data |
| **OLED SCL** | GPIO **22** | I2C Clock |
| **NeoPixel** | GPIO **48** | **Status & Life Indicator** |
| **SD CS** | GPIO **7** | **Custom HSPI** |
| **SD MOSI** | GPIO **6** | **Custom HSPI** |
| **SD MISO** | GPIO **4** | **Custom HSPI** |
| **SD CLK** | GPIO **5** | **Custom HSPI** |

## LED Indicators

1.  **Start Up**:
    - **Green**: SD Card Detected OK.
    - **Orange**: SD Card Failed (Simulation continues anyway).
 * Libraries:
 *   - U8g2
 *   - Adafruit_NeoPixel
 *   - SD, FS, SPI
 *
 * Author: Alejandro Rebolledo (arebolledo@udd.cl)
 * Date: 2025-12-19
 * License: CC BY-NC 4.0
 * Original Idea: https://elibaum.com/2021/03/25/pandemic-projects-tiny-life.html
 */
    - **White Flash**: Reset/New Game.
2.  **During Game**:
    - **Blue-ish**: Low population density (Classic Life).
    - **Purple/Red**: High population density (Explosion/Chaos).
    - **Pulsing**: The Green component pulses with each generation step.

## Usage

1. **Power On**: LED shows status. Screen shows "Select Mode".
2. **Select Mode**:
   - Press **A** for **Scale 1** (128x64).
   - Press **B** for **Scale 2** (64x32).
3. **Running**:
   - Simulation runs automatically. Loop detections trigger auto-resets after logging.
4. **Controls**:
   - **Button A**: Manual Reset.
   - **Button B**: Enter Deep Sleep.

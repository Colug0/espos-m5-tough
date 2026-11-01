/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * M5Stack Tough (SKU K034) wiring. Sources: the M5Stack docs page, and where it
 * is ambiguous, M5GFX/M5Unified (MIT), which run on every Tough sold, and
 * bareboat-necessities/bbn-m5stack-tough (MIT).
 */
#pragma once

#include "driver/gpio.h"

namespace tough_hal {

/// Passed to espos_start() as opts.board. The registry's reportedAs must
/// equal it exactly, so it never changes.
inline constexpr const char* kBoardName = "M5Stack Tough";

// Internal I2C: AXP192 (0x34), CHSC6540 touch (0x2E), BM8563 RTC (0x51).
inline constexpr gpio_num_t kI2cSda = GPIO_NUM_21;
inline constexpr gpio_num_t kI2cScl = GPIO_NUM_22;
inline constexpr uint32_t kI2cHz = 400000;
inline constexpr uint8_t kAxp192Addr = 0x34;
inline constexpr uint8_t kTouchAddr = 0x2E;

// LCD on SPI2 (shared with the microSD slot, CS 4). No reset pin: the AXP192
// drives LCD reset (its GPIO4) and the touch reset (its GPIO1).
inline constexpr gpio_num_t kSpiMosi = GPIO_NUM_23;
inline constexpr gpio_num_t kSpiMiso = GPIO_NUM_38;
inline constexpr gpio_num_t kSpiSclk = GPIO_NUM_18;
inline constexpr gpio_num_t kLcdCs = GPIO_NUM_5;
inline constexpr gpio_num_t kLcdDc = GPIO_NUM_15;
inline constexpr gpio_num_t kSdCs = GPIO_NUM_4;
inline constexpr uint32_t kLcdHz = 40000000;
inline constexpr uint16_t kLcdWidth = 320;
inline constexpr uint16_t kLcdHeight = 240;

inline constexpr gpio_num_t kTouchInt = GPIO_NUM_39;  // low while touched

// NS4168 I2S amplifier; enabled through AXP192 GPIO2.
inline constexpr gpio_num_t kI2sBclk = GPIO_NUM_12;
inline constexpr gpio_num_t kI2sWs = GPIO_NUM_0;
inline constexpr gpio_num_t kI2sDout = GPIO_NUM_2;

}  // namespace tough_hal

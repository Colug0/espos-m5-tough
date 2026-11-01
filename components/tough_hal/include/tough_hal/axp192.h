/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The Tough's AXP192 PMIC, reduced to what the board wires to it:
 *   LDO2  LCD logic + microSD power (3.3 V)
 *   LDO3  LCD backlight; brightness is its voltage, dark below 2.5 V
 *   GPIO4 LCD reset (active low)
 *   GPIO1 touch controller reset (active low)
 *   GPIO2 NS4168 speaker amplifier enable
 * Register use follows M5GFX (board bring-up, Light_M5Tough) and M5Unified
 * (Power_Class, AXP192_Class), both MIT.
 */
#pragma once

#include <cstdint>

#include "esp_err.h"

namespace tough_hal {
namespace axp192 {

/// Check the chip id, power the LCD (LDO2), configure the reset and speaker
/// GPIOs and switch the unused DCDC3 off. The backlight stays off and the
/// speaker disabled until asked.
esp_err_t init();

/// Pulse the LCD and touch reset lines, then wait until the panel accepts
/// commands again. Returns the first failed I2C write; the lines may then be
/// left low.
esp_err_t reset_lcd_and_touch();

/// Backlight 0-100 %. 0 switches LDO3 off; 1-100 maps onto the 2.5-3.3 V
/// range in which the backlight is lit.
void set_backlight(uint8_t pct);

/// Enable or disable the speaker amplifier.
void set_speaker(bool on);

}  // namespace axp192
}  // namespace tough_hal

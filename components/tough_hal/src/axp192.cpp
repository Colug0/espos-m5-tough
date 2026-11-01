/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
#include "tough_hal/axp192.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tough_hal/board.h"
#include "tough_hal/i2c_bus.h"

namespace tough_hal {
namespace axp192 {

namespace {

const char* TAG = "axp192";
constexpr int kTimeoutMs = 50;

// Registers (AXP192 datasheet).
constexpr uint8_t kRegChipId = 0x03;      // reads 0x03 on an AXP192
constexpr uint8_t kRegPowerCtrl = 0x12;   // output enables
constexpr uint8_t kRegLdo23Volt = 0x28;   // LDO2 high nibble, LDO3 low nibble
constexpr uint8_t kRegGpio1Ctrl = 0x92;
constexpr uint8_t kRegGpio2Ctrl = 0x93;
constexpr uint8_t kRegGpio012Out = 0x94;
constexpr uint8_t kRegGpio34Ctrl = 0x95;
constexpr uint8_t kRegGpio34Out = 0x96;

constexpr uint8_t kEnDcdc3 = 0x02;
constexpr uint8_t kEnLdo2 = 0x04;
constexpr uint8_t kEnLdo3 = 0x08;

i2c_master_dev_handle_t dev() { return i2c_device(kAxp192Addr); }

esp_err_t read(uint8_t reg, uint8_t* val) {
  return i2c_master_transmit_receive(dev(), &reg, 1, val, 1, kTimeoutMs);
}

esp_err_t write(uint8_t reg, uint8_t val) {
  const uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(dev(), buf, sizeof(buf), kTimeoutMs);
}

/// new = (old & keep) | set
esp_err_t update(uint8_t reg, uint8_t keep, uint8_t set) {
  uint8_t v = 0;
  esp_err_t err = read(reg, &v);
  if (err != ESP_OK) return err;
  return write(reg, static_cast<uint8_t>((v & keep) | set));
}

}  // namespace

esp_err_t init() {
  uint8_t id = 0;
  esp_err_t err = read(kRegChipId, &id);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "no answer at 0x%02x: %s", kAxp192Addr, esp_err_to_name(err));
    return err;
  }
  if (id != 0x03) {
    ESP_LOGE(TAG, "chip id 0x%02x, not an AXP192", id);
    return ESP_ERR_NOT_FOUND;
  }
  // GPIO4 as NMOS open-drain output (LCD reset).
  update(kRegGpio34Ctrl, 0x72, 0x84);
  // LDO2 = 3.3 V (1.8 V + 15 x 0.1 V), LDO3 nibble untouched, then enable.
  update(kRegLdo23Volt, 0x0F, 0xF0);
  update(kRegPowerCtrl, 0xFF, kEnLdo2);
  // GPIO1 as NMOS open-drain output (touch reset).
  update(kRegGpio1Ctrl, 0xF8, 0x00);
  // DCDC3 drives the Core2's backlight; on the Tough it is unused.
  update(kRegPowerCtrl, static_cast<uint8_t>(~kEnDcdc3), 0x00);
  set_speaker(false);
  ESP_LOGI(TAG, "LCD power on");
  return ESP_OK;
}

void reset_lcd_and_touch() {
  update(kRegGpio34Out, 0xFD, 0x00);   // GPIO4 low
  update(kRegGpio012Out, 0xFD, 0x00);  // GPIO1 low
  vTaskDelay(pdMS_TO_TICKS(2));
  update(kRegGpio34Out, 0xFF, 0x02);
  update(kRegGpio012Out, 0xFF, 0x02);
  // The panel reloads its registers for up to 120 ms after a reset
  // (ILI9342E datasheet 12.4); commands sent earlier may be lost.
  vTaskDelay(pdMS_TO_TICKS(120));
}

void set_backlight(uint8_t pct) {
  if (pct == 0) {
    update(kRegPowerCtrl, static_cast<uint8_t>(~kEnLdo3), 0x00);
    return;
  }
  if (pct > 100) pct = 100;
  // LDO3 = 1.8 V + N x 0.1 V; the backlight lights from N = 7 (2.5 V).
  const uint8_t n = static_cast<uint8_t>(7 + (pct * 8 + 50) / 100);
  update(kRegLdo23Volt, 0xF0, n);
  update(kRegPowerCtrl, 0xFF, kEnLdo3);
}

void set_speaker(bool on) {
  // 0x06 leaves GPIO2 floating, pulled up on the board: amplifier on.
  // 0x05 drives it low: amplifier off.
  write(kRegGpio2Ctrl, on ? 0x06 : 0x05);
}

}  // namespace axp192
}  // namespace tough_hal

/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * CHSC6540 on I2C 0x2E. Protocol as M5GFX's Touch_CHSC6540 (MIT): write
 * 5Ah 5Ah once to switch the interrupt mode, then read from register 02h a
 * point count followed by 12-bit X/Y per point. The INT line (GPIO 39) is low
 * while a finger is down, so an idle panel costs no I2C traffic.
 */
#include "esp_log.h"
#include "tough_hal/board.h"
#include "tough_hal/i2c_bus.h"
#include "tough_hal/tough_display.h"

namespace tough_hal {

namespace {
const char* TAG = "touch";
constexpr int kTimeoutMs = 20;
}  // namespace

void ToughTouch::init() {
  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << kTouchInt;
  io.mode = GPIO_MODE_INPUT;  // GPIO 39 has no internal pull; the board pulls up
  gpio_config(&io);

  const uint8_t irq_mode[2] = {0x5A, 0x5A};
  esp_err_t err = i2c_master_transmit(i2c_device(kTouchAddr), irq_mode,
                                      sizeof(irq_mode), kTimeoutMs);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "CHSC6540 did not answer: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "CHSC6540 ready");
  }
}

TouchDriver::TouchPoint ToughTouch::read() {
  if (gpio_get_level(kTouchInt) != 0) {
    last_.pressed = false;
    return last_;
  }
  const uint8_t reg = 0x02;
  uint8_t d[5] = {};
  esp_err_t err = i2c_master_transmit_receive(i2c_device(kTouchAddr), &reg, 1,
                                              d, sizeof(d), kTimeoutMs);
  if (err != ESP_OK || d[0] == 0) {
    last_.pressed = false;
    return last_;
  }
  uint16_t x = static_cast<uint16_t>(((d[1] & 0x0F) << 8) | d[2]);
  uint16_t y = static_cast<uint16_t>(((d[3] & 0x0F) << 8) | d[4]);
  if (x >= kLcdWidth) x = kLcdWidth - 1;
  if (y >= kLcdHeight) y = kLcdHeight - 1;
  last_ = {x, y, true};
  return last_;
}

}  // namespace tough_hal

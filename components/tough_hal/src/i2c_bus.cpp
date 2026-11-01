/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
#include "tough_hal/i2c_bus.h"

#include <mutex>

#include "esp_log.h"
#include "tough_hal/board.h"

namespace tough_hal {

namespace {
const char* TAG = "i2c";
std::mutex s_mutex;
i2c_master_bus_handle_t s_bus = nullptr;
struct Dev {
  uint8_t addr;
  i2c_master_dev_handle_t handle;
};
Dev s_devs[4];
size_t s_ndevs = 0;
}  // namespace

i2c_master_bus_handle_t i2c_bus() {
  std::lock_guard<std::mutex> g(s_mutex);
  if (s_bus) return s_bus;
  i2c_master_bus_config_t cfg = {};
  cfg.i2c_port = I2C_NUM_1;
  cfg.sda_io_num = kI2cSda;
  cfg.scl_io_num = kI2cScl;
  cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  cfg.glitch_ignore_cnt = 7;
  // The board has its own pull-ups; the internal ones only add margin.
  cfg.flags.enable_internal_pullup = true;
  ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_bus));
  return s_bus;
}

i2c_master_dev_handle_t i2c_device(uint8_t addr) {
  i2c_master_bus_handle_t bus = i2c_bus();
  std::lock_guard<std::mutex> g(s_mutex);
  for (size_t i = 0; i < s_ndevs; i++) {
    if (s_devs[i].addr == addr) return s_devs[i].handle;
  }
  if (s_ndevs == sizeof(s_devs) / sizeof(s_devs[0])) {
    ESP_LOGE(TAG, "no device slot left for 0x%02x", addr);
    return nullptr;
  }
  i2c_device_config_t dc = {};
  dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dc.device_address = addr;
  dc.scl_speed_hz = kI2cHz;
  i2c_master_dev_handle_t h = nullptr;
  ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &h));
  s_devs[s_ndevs++] = {addr, h};
  return h;
}

}  // namespace tough_hal

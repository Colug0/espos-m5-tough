/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
#include "tough_hal/tough_display.h"

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tough_hal/axp192.h"
#include "tough_hal/board.h"

namespace tough_hal {

namespace {

const char* TAG = "display";

// 10 lines per strip: 6.4 KB each, two of them, all internal RAM. 20 lines
// cost 12.8 KB more of the pool that ran out with the player, the Signal K
// stream and the designer's mirror running; at 320 px a full redraw is 24
// strips either way, so the panel is no slower to the eye.
constexpr int kStripLines = 10;

struct InitCmd {
  uint8_t cmd;
  uint8_t len;
  uint8_t data[15];
  uint16_t delay_ms;
};

// Init lists from M5GFX lgfx/v1/panel/Panel_ILI9342.hpp (MIT, M5Stack /
// LovyanGFX), the sequences M5Stack ships for the Core2 family. Pixel format,
// orientation and inversion follow below, as M5GFX sends them after the list.
#if CONFIG_TOUGH_PANEL_ILI9342E
const InitCmd kInit[] = {
    {0xDD, 1, {0x01}, 0},  // Set EXTC (E command set)
    {0xD5, 1, {0x00}, 0},
    {0xB1, 1, {0x22}, 0},
    {0xC8, 1, {0x38}, 0},  // Power Control 3 (VCOM)
    {0xCB, 1, {0x1C}, 0},
    {0xC9, 1, {0x1A}, 0},
    {0xCA, 1, {0x1A}, 0},
    {0xB7, 4, {0x5A, 0x41, 0x11, 0x19}, 0},
    {0xE4, 15, {0x04, 0x08, 0x11, 0x06, 0x12, 0x07, 0x3A, 0x76, 0x47, 0x07, 0x0F, 0x0A, 0x11, 0x19, 0x05}, 0},
    {0xE5, 15, {0x02, 0x03, 0x07, 0x06, 0x12, 0x07, 0x36, 0x5F, 0x48, 0x06, 0x10, 0x0C, 0x16, 0x14, 0x09}, 0},
    {0x38, 0, {}, 0},    // Idle mode off
    {0x29, 0, {}, 0},    // Display on
    {0x11, 0, {}, 120},  // Sleep out
};
#else
const InitCmd kInit[] = {
    {0xC8, 3, {0xFF, 0x93, 0x42}, 0},  // Set EXTC
    {0xC0, 2, {0x12, 0x12}, 0},        // Power control 1
    {0xC1, 1, {0x03}, 0},              // Power control 2
    {0xC5, 1, {0xF2}, 0},              // VCOM control 1
    {0xB0, 1, {0xE0}, 0},
    {0xF6, 3, {0x01, 0x00, 0x00}, 0},
    {0xE0, 15, {0x00, 0x0C, 0x11, 0x04, 0x11, 0x08, 0x37, 0x89, 0x4C, 0x06, 0x0C, 0x0A, 0x2E, 0x34, 0x0F}, 0},
    {0xE1, 15, {0x00, 0x0B, 0x11, 0x05, 0x13, 0x09, 0x33, 0x67, 0x48, 0x07, 0x0E, 0x0B, 0x2E, 0x33, 0x0F}, 0},
    {0xB6, 4, {0x08, 0x82, 0x1D, 0x04}, 0},  // Display function control
    {0x38, 0, {}, 0},
    {0x29, 0, {}, 0},
    {0x11, 0, {}, 120},
};
#endif

#if CONFIG_TOUGH_PANEL_ILI9342E
constexpr const char* kPanelName = "ILI9342E";
#else
constexpr const char* kPanelName = "ILI9342C";
#endif

constexpr uint8_t kCmdCaset = 0x2A;
constexpr uint8_t kCmdRaset = 0x2B;
constexpr uint8_t kCmdRamwr = 0x2C;
constexpr uint8_t kCmdMadctl = 0x36;
constexpr uint8_t kCmdColmod = 0x3A;
constexpr uint8_t kCmdInvon = 0x21;
constexpr uint8_t kCmdDispoff = 0x28;
constexpr uint8_t kCmdDispon = 0x29;

// MADCTL: native 320x240 landscape, BGR panel. M5GFX lands on the same value
// (offset_rotation 3 + rotation 1, rgb_order false).
constexpr uint8_t kMadctlBgr = 0x08;
constexpr uint8_t kColmod16bit = 0x55;

}  // namespace

void ToughDisplay::init() {
  buf_size_ = kLcdWidth * kStripLines * sizeof(uint16_t);
  for (auto& b : buf_) {
    b = heap_caps_malloc(buf_size_, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!b) {
      ESP_LOGE(TAG, "no internal DMA memory for a %u byte draw buffer",
               static_cast<unsigned>(buf_size_));
      abort();
    }
  }

  spi_bus_config_t bus = {};
  bus.mosi_io_num = kSpiMosi;
  bus.miso_io_num = kSpiMiso;
  bus.sclk_io_num = kSpiSclk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = static_cast<int>(buf_size_);
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

  // The microSD card on the same bus must not answer to LCD traffic.
  gpio_set_direction(kSdCs, GPIO_MODE_OUTPUT);
  gpio_set_level(kSdCs, 1);

  done_ = xSemaphoreCreateBinary();
  esp_lcd_panel_io_spi_config_t io = {};
  io.cs_gpio_num = kLcdCs;
  io.dc_gpio_num = kLcdDc;
  io.spi_mode = 0;
  io.pclk_hz = kLcdHz;
  io.trans_queue_depth = 4;
  io.on_color_trans_done = &ToughDisplay::on_trans_done;
  io.user_ctx = this;
  io.lcd_cmd_bits = 8;
  io.lcd_param_bits = 8;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
      static_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST), &io, &io_));

  send_init_sequence();
  ESP_LOGI(TAG, "%s %ux%u ready", kPanelName, kLcdWidth, kLcdHeight);
}

void ToughDisplay::send_init_sequence() {
  for (const auto& c : kInit) {
    esp_lcd_panel_io_tx_param(io_, c.cmd, c.len ? c.data : nullptr, c.len);
    if (c.delay_ms) vTaskDelay(pdMS_TO_TICKS(c.delay_ms));
  }
  const uint8_t colmod = kColmod16bit;
  esp_lcd_panel_io_tx_param(io_, kCmdColmod, &colmod, 1);
  const uint8_t madctl = kMadctlBgr;
  esp_lcd_panel_io_tx_param(io_, kCmdMadctl, &madctl, 1);
  // The Tough's IPS panel shows true colours inverted.
  esp_lcd_panel_io_tx_param(io_, kCmdInvon, nullptr, 0);
}

uint16_t ToughDisplay::width() const { return kLcdWidth; }
uint16_t ToughDisplay::height() const { return kLcdHeight; }

void* ToughDisplay::get_draw_buffer(int index) {
  return (index == 0 || index == 1) ? buf_[index] : nullptr;
}

size_t ToughDisplay::get_draw_buffer_size() { return buf_size_; }

bool ToughDisplay::on_trans_done(esp_lcd_panel_io_handle_t,
                                 esp_lcd_panel_io_event_data_t*, void* ctx) {
  auto* self = static_cast<ToughDisplay*>(ctx);
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(self->done_, &woken);
  return woken == pdTRUE;
}

void ToughDisplay::flush(int x, int y, int w, int h, const void* buf) {
  const int x2 = x + w - 1;
  const int y2 = y + h - 1;
  const uint8_t col[4] = {static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(x),
                          static_cast<uint8_t>(x2 >> 8), static_cast<uint8_t>(x2)};
  const uint8_t row[4] = {static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y),
                          static_cast<uint8_t>(y2 >> 8), static_cast<uint8_t>(y2)};
  esp_lcd_panel_io_tx_param(io_, kCmdCaset, col, sizeof(col));
  esp_lcd_panel_io_tx_param(io_, kCmdRaset, row, sizeof(row));
  pending_ = true;
  esp_lcd_panel_io_tx_color(io_, kCmdRamwr, buf,
                            static_cast<size_t>(w) * h * sizeof(uint16_t));
}

void ToughDisplay::wait_flush_done() {
  if (!pending_) return;
  xSemaphoreTake(done_, portMAX_DELAY);
  pending_ = false;
}

void ToughDisplay::set_brightness(uint8_t pct) { axp192::set_backlight(pct); }

void ToughDisplay::set_display_on(bool on) {
  esp_lcd_panel_io_tx_param(io_, on ? kCmdDispon : kCmdDispoff, nullptr, 0);
}

}  // namespace tough_hal

/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tough_hal/display_driver.h"

namespace tough_hal {

/// The Tough's 320x240 ILI9342C/E on SPI2, 40 MHz, RGB565 big-endian.
/// Expects axp192::init() to have powered the panel. flush() queues a DMA
/// transfer and returns; wait_flush_done() blocks until it has finished.
class ToughDisplay : public DisplayDriver {
 public:
  void init() override;
  uint16_t width() const override;
  uint16_t height() const override;
  /// Two strip buffers in internal DMA memory: the ESP32's SPI DMA cannot
  /// read PSRAM.
  void* get_draw_buffer(int index) override;
  size_t get_draw_buffer_size() override;
  void flush(int x, int y, int w, int h, const void* buf) override;
  void wait_flush_done() override;
  void set_brightness(uint8_t pct) override;
  void set_display_on(bool on) override;

 private:
  static bool on_trans_done(esp_lcd_panel_io_handle_t io,
                            esp_lcd_panel_io_event_data_t* edata, void* ctx);
  void send_init_sequence();

  esp_lcd_panel_io_handle_t io_ = nullptr;
  SemaphoreHandle_t done_ = nullptr;
  bool pending_ = false;
  void* buf_[2] = {nullptr, nullptr};
  size_t buf_size_ = 0;
};

class ToughTouch : public TouchDriver {
 public:
  /// Expects axp192::reset_lcd_and_touch() to have released the reset.
  void init() override;
  TouchPoint read() override;

 private:
  TouchPoint last_ = {0, 0, false};
};

}  // namespace tough_hal

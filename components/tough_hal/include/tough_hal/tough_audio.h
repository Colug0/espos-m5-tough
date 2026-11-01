/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <atomic>

#include "driver/i2s_std.h"
#include "espos_audio/audio_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace tough_hal {

/// The NS4168 class-D amplifier on I2S0 (BCLK 12, WS 0, DATA 2), 16 kHz mono.
/// The NS4168 has no volume control, so volume scales the samples. The
/// amplifier is switched on (AXP192 GPIO2) only while a clip plays, which
/// keeps an idle speaker silent.
class ToughAudio : public espos_audio::AudioDriver {
 public:
  void init() override;
  bool ready() const override { return ready_.load(); }
  uint32_t sample_rate() const override { return kRate; }
  void play_pcm(const int16_t* samples, size_t frames) override;
  void set_volume(uint8_t pct) override { volume_ = pct > 100 ? 100 : pct; }
  void set_enabled(bool on) override { enabled_ = on; }

 private:
  static constexpr uint32_t kRate = 16000;
  struct Clip {
    int16_t* samples;
    size_t frames;
  };
  static void task(void* arg);
  void run();

  i2s_chan_handle_t tx_ = nullptr;
  QueueHandle_t queue_ = nullptr;
  size_t ring_frames_ = 0;  // frames the I2S DMA ring holds
  std::atomic<bool> ready_{false};
  std::atomic<uint8_t> volume_{80};
  std::atomic<bool> enabled_{true};
};

}  // namespace tough_hal

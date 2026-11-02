/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <atomic>

#include "driver/i2s_std.h"
#include "espos_audio/audio_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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
  static constexpr size_t kQueueLen = 4;
  struct Clip {
    int16_t* samples;
    size_t frames;
  };
  static void task(void* arg);
  void run();
  bool pop(Clip* out);
  size_t pending();

  i2s_chan_handle_t tx_ = nullptr;
  // Clips waiting to play. Producers and the audio task touch it only under
  // lock_, so "full, drop the oldest" is one step that the audio task cannot
  // interleave with; the task sleeps on a task notification.
  SemaphoreHandle_t lock_ = nullptr;
  TaskHandle_t task_ = nullptr;
  Clip ring_[kQueueLen] = {};
  size_t head_ = 0;
  size_t count_ = 0;
  size_t ring_frames_ = 0;  // frames the I2S DMA ring holds
  std::atomic<bool> ready_{false};
  std::atomic<uint8_t> volume_{100};
  std::atomic<bool> enabled_{true};
};

}  // namespace tough_hal

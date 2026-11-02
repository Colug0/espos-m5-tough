/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
#include "tough_hal/tough_audio.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "tough_hal/axp192.h"
#include "tough_hal/board.h"

namespace tough_hal {

namespace {
const char* TAG = "audio";
// i2s_channel_write() returns once samples are in the DMA ring, not once they
// have played. Before the amplifier goes off, a whole ring of silence plus
// 20 ms is written, so the clip's tail has left the ring. Written in chunks of
// this size.
constexpr size_t kTailFrames = 320;
const int16_t kSilence[kTailFrames] = {};
}  // namespace

void ToughAudio::init() {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.auto_clear = true;  // an empty DMA ring sends zeros, not the last clip
  ring_frames_ = static_cast<size_t>(chan.dma_desc_num) * chan.dma_frame_num;
  esp_err_t err = i2s_new_channel(&chan, &tx_, nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
    return;
  }

  i2s_std_config_t std = {};
  std.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate);
  std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_MONO);
  // The NS4168 plays one channel chosen by its CTRL pin; sending the mono
  // samples on both slots works whichever the board selected.
  std.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  std.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  std.gpio_cfg.bclk = kI2sBclk;
  std.gpio_cfg.ws = kI2sWs;
  std.gpio_cfg.dout = kI2sDout;
  std.gpio_cfg.din = I2S_GPIO_UNUSED;
  err = i2s_channel_init_std_mode(tx_, &std);
  if (err == ESP_OK) err = i2s_channel_enable(tx_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s std mode: %s", esp_err_to_name(err));
    return;
  }

  lock_ = xSemaphoreCreateMutex();
  if (!lock_ ||
      xTaskCreatePinnedToCore(&ToughAudio::task, "audio", 3072, this, 4,
                              &task_, 1) != pdPASS) {
    ESP_LOGE(TAG, "no memory for the audio task");
    return;
  }
  ready_ = true;
  ESP_LOGI(TAG, "NS4168 ready, %u Hz mono", static_cast<unsigned>(kRate));
}

void ToughAudio::play_pcm(const int16_t* samples, size_t frames) {
  if (!ready_ || !enabled_ || !samples || frames == 0) return;
  // i2s_channel_write copies into its own DMA buffers, so the clip itself may
  // live in PSRAM.
  auto* copy = static_cast<int16_t*>(heap_caps_malloc(
      frames * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!copy) copy = static_cast<int16_t*>(malloc(frames * sizeof(int16_t)));
  if (!copy) return;
  const int32_t vol = volume_;
  for (size_t i = 0; i < frames; i++) {
    copy[i] = static_cast<int16_t>(samples[i] * vol / 100);
  }
  // A full ring drops the oldest waiting clip, not this one: the newest
  // chime belongs to the latest escalation, which may be an emergency
  // arriving behind a run of alarms.
  Clip evicted = {nullptr, 0};
  xSemaphoreTake(lock_, portMAX_DELAY);
  if (count_ == kQueueLen) {
    evicted = ring_[head_];
    head_ = (head_ + 1) % kQueueLen;
    --count_;
  }
  ring_[(head_ + count_) % kQueueLen] = {copy, frames};
  ++count_;
  xSemaphoreGive(lock_);
  free(evicted.samples);
  xTaskNotifyGive(task_);
}

bool ToughAudio::pop(Clip* out) {
  xSemaphoreTake(lock_, portMAX_DELAY);
  const bool any = count_ > 0;
  if (any) {
    *out = ring_[head_];
    head_ = (head_ + 1) % kQueueLen;
    --count_;
  }
  xSemaphoreGive(lock_);
  return any;
}

size_t ToughAudio::pending() {
  xSemaphoreTake(lock_, portMAX_DELAY);
  const size_t n = count_;
  xSemaphoreGive(lock_);
  return n;
}

void ToughAudio::task(void* arg) { static_cast<ToughAudio*>(arg)->run(); }

void ToughAudio::run() {
  bool amp_on = false;
  // amp_on tracks what the AXP192 confirmed. A failed switch-on is retried
  // with the next clip; a failed switch-off once a second until it holds.
  for (;;) {
    Clip clip;
    if (!pop(&clip)) {
      // A clip queued after pop() leaves a notification pending, so this
      // returns at once rather than missing it.
      if (ulTaskNotifyTake(pdTRUE, amp_on ? pdMS_TO_TICKS(1000) : portMAX_DELAY) == 0 &&
          axp192::set_speaker(false) == ESP_OK) {
        amp_on = false;
      }
      continue;
    }
    if (!amp_on && enabled_) {
      if (axp192::set_speaker(true) == ESP_OK) {
        amp_on = true;
      } else {
        ESP_LOGW(TAG, "could not switch the amplifier on");
      }
    }
    size_t written = 0;
    if (enabled_) {
      i2s_channel_write(tx_, clip.samples, clip.frames * sizeof(int16_t),
                        &written, portMAX_DELAY);
    }
    free(clip.samples);
    if (pending() == 0 && amp_on) {
      for (size_t left = ring_frames_ + kTailFrames; left > 0;) {
        const size_t n = left < kTailFrames ? left : kTailFrames;
        i2s_channel_write(tx_, kSilence, n * sizeof(int16_t), &written,
                          portMAX_DELAY);
        left -= n;
      }
      if (axp192::set_speaker(false) == ESP_OK) {
        amp_on = false;
      } else {
        ESP_LOGW(TAG, "could not switch the amplifier off");
      }
    }
  }
}

}  // namespace tough_hal

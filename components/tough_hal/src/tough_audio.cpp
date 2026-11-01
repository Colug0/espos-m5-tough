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
constexpr int kQueueLen = 4;
// 20 ms of silence after the last clip, so the DMA ring has played the
// clip's tail before the amplifier goes off.
constexpr size_t kTailFrames = 320;
const int16_t kSilence[kTailFrames] = {};
}  // namespace

void ToughAudio::init() {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.auto_clear = true;  // an empty DMA ring sends zeros, not the last clip
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

  queue_ = xQueueCreate(kQueueLen, sizeof(Clip));
  if (!queue_ ||
      xTaskCreatePinnedToCore(&ToughAudio::task, "audio", 3072, this, 4,
                              nullptr, 1) != pdPASS) {
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
  Clip clip = {copy, frames};
  if (xQueueSend(queue_, &clip, 0) != pdTRUE) free(copy);  // busy: drop it
}

void ToughAudio::task(void* arg) { static_cast<ToughAudio*>(arg)->run(); }

void ToughAudio::run() {
  bool amp_on = false;
  for (;;) {
    Clip clip;
    xQueueReceive(queue_, &clip, portMAX_DELAY);
    if (!amp_on && enabled_) {
      axp192::set_speaker(true);
      amp_on = true;
    }
    size_t written = 0;
    if (enabled_) {
      i2s_channel_write(tx_, clip.samples, clip.frames * sizeof(int16_t),
                        &written, portMAX_DELAY);
    }
    free(clip.samples);
    if (uxQueueMessagesWaiting(queue_) == 0 && amp_on) {
      i2s_channel_write(tx_, kSilence, sizeof(kSilence), &written,
                        portMAX_DELAY);
      axp192::set_speaker(false);
      amp_on = false;
    }
  }
}

}  // namespace tough_hal

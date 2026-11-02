/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/* From espos-p4-cockpit main/jlp/audio/chime.cpp, changed for the M5Stack Tough (main/jlp/README.md). */
#include "chime.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "esp_log.h"

#include "jlp/layout/store.h"

static const char* TAG = "jlp.chime";

namespace jlp {

// NVS key for the persisted chime mute (keys cap at 15 chars).
static constexpr const char* kChimeMutedKey = "chime_muted";

void Chime::init(espos_audio::AudioDriver* audio) {
  audio_ = audio;
  // Restore before any notification can fire, so a muted helm stays quiet
  // through the boot chime as well.
  muted_.store(store_flag_get(kChimeMutedKey, false));
}

void Chime::set_muted(bool m) {
  muted_ = m;
  store_flag_set(kChimeMutedKey, m);
}

namespace {

// A single tone segment: frequency and duration. A zero-frequency
// segment is a rest (silence between beeps).
struct Seg {
  float hz;
  uint16_t ms;
};

// The Tough's 1 W speaker is small: below about 1.5 kHz it gives little,
// and the ear is most sensitive around 2-3 kHz, so the tones sit there.
// Each tone is a sine pushed towards a square by tanh: more energy at the
// same peak, still without the hard edges that would click.
constexpr float kDrive = 3.0f;

// Append a tone (or a rest, when hz==0) to `out` at `rate`, with a
// short linear fade in/out so the speaker doesn't click on each edge.
void append_seg(std::vector<int16_t>& out, uint32_t rate, const Seg& seg,
                float amp) {
  size_t n = (size_t)((uint32_t)seg.ms * rate / 1000);
  if (n == 0) return;
  size_t fade = n / 20;  // ~5% ramp on each end
  if (fade == 0) fade = 1;
  const float two_pi_f = 2.0f * (float)M_PI * seg.hz / (float)rate;
  const float norm = 1.0f / tanhf(kDrive);
  size_t base = out.size();
  out.resize(base + n);
  for (size_t i = 0; i < n; ++i) {
    float env = 1.0f;
    if (i < fade)
      env = (float)i / (float)fade;
    else if (i >= n - fade)
      env = (float)(n - i) / (float)fade;
    float s = (seg.hz > 0.0f) ? tanhf(kDrive * sinf(two_pi_f * (float)i)) * norm
                              : 0.0f;
    out[base + i] = (int16_t)(s * env * amp * 32767.0f);
  }
}

// Severity -> tone pattern. Higher severity = more insistent (more
// beeps, tighter spacing, higher pitch). The NS4168 has no gain setting,
// so the level is set here: alarms at full scale, the lower states
// quieter so they stay distinguishable.
void pattern_for(NotState state, std::vector<Seg>& segs, float& amp) {
  switch (state) {
    case NotState::Emergency:
      amp = 1.0f;
      segs = {{2600, 140}, {0, 70}, {2600, 140}, {0, 70},
              {2600, 140}, {0, 70}, {2600, 140}};
      break;
    case NotState::Alarm:
      amp = 1.0f;
      segs = {{2400, 220}, {0, 110}, {2400, 220}, {0, 110}, {2400, 220}};
      break;
    case NotState::Warn:
      amp = 0.8f;
      segs = {{2000, 250}, {0, 120}, {2000, 250}};
      break;
    case NotState::Alert:
      amp = 0.6f;
      segs = {{1600, 250}};
      break;
    default:  // Nominal / Normal — silent
      break;
  }
}

}  // namespace

void Chime::play(NotState state, bool ignore_mute) {
  if ((muted_ && !ignore_mute) || !audio_ || !audio_->ready()) return;

  std::vector<Seg> segs;
  float amp = 0.0f;
  pattern_for(state, segs, amp);
  if (segs.empty()) return;

  // Trailing silence so the codec's I2S DMA fully drains the last tone
  // before the driver closes the device and drops PA_CTRL — otherwise
  // the final few ms get clipped as the amp cuts.
  segs.push_back({0, 40});

  const uint32_t rate = audio_->sample_rate();
  std::vector<int16_t> pcm;
  pcm.reserve(rate);  // about a second worst case
  for (const Seg& seg : segs) append_seg(pcm, rate, seg, amp);

  ESP_LOGI(TAG, "chime %s (%u frames)", not_state_name(state),
           (unsigned)pcm.size());
  audio_->play_pcm(pcm.data(), pcm.size());
}

Chime& chime() {
  static Chime c;
  return c;
}

}  // namespace jlp

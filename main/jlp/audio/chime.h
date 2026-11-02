/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/* From espos-p4-cockpit main/jlp/audio/chime.h, changed for the M5Stack Tough (main/jlp/README.md). */
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "espos_audio/audio_driver.h"

#include "../notifications_registry.h"

namespace jlp {

/// Synthesises short alert tones and plays them through the panel's
/// audio driver. Severity-shaped: a warn is a single soft beep, an
/// alarm an urgent double, an emergency a fast triple. No assets — the
/// PCM is generated on the fly, so this costs no flash beyond the code.
///
/// Process-lifetime singleton (like the alert overlay). init() is
/// called once at boot with the board's AudioDriver; play() is safe to
/// call from the UI task because the driver's play_pcm()
/// is non-blocking.
class Chime {
 public:
  /// Wire in the board audio sink. Null is tolerated (a board without
  /// a speaker) — play() then no-ops.
  // Wires the audio driver and restores the persisted mute state. Call
  // after store_init(); the flag shares the layout's NVS namespace.
  void init(espos_audio::AudioDriver* audio);

  /// Play the tone for `state`. Nominal/normal are silent. Suppressed
  /// while muted unless `ignore_mute` (the /beep diagnostic forces one).
  void play(NotState state, bool ignore_mute = false);

  /// True if a working audio sink is wired (for the /hello status +
  /// /beep probe). False on a speaker-less board or a failed codec init.
  bool audio_ready() const {
    auto* a = audio_.load(std::memory_order_acquire);
    return a && a->ready();
  }

  /// Force an alarm-severity tone regardless of gating. Drives the
  /// /beep diagnostic endpoint so audibility can be tested over HTTP
  /// without waiting for a real notification.
  void test() { play(NotState::Alarm, /*ignore_mute=*/true); }

  /// Panel-local standing mute. While muted, play() no-ops — the alert
  /// overlay still shows alarms, but this panel stays silent (current
  /// AND future) until un-muted. Local to this panel; doesn't touch SK.
  /// Atomic: written on the UI task (the toggle) but also read on the
  /// httpd task via /beep -> test() -> play().
  // Persisted: a helm silenced before a reboot must not start beeping
  // again on its own.
  void set_muted(bool m);
  bool muted() const { return muted_; }

 private:
  // Published by init() only after clips_ is built: /beep can call play()
  // from the httpd task while the UI task is still in init().
  std::atomic<espos_audio::AudioDriver*> audio_{nullptr};
  std::atomic_bool muted_{false};
  // One clip per sounding state (Alert..Emergency), synthesised once in
  // init() and read-only afterwards, so play() needs no lock and costs the
  // UI task nothing beyond handing the clip over.
  std::vector<int16_t> clips_[4];
};

Chime& chime();

}  // namespace jlp

/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/* From espos-p4-cockpit main/jlp/sun_state.h, changed for the M5Stack Tough (main/jlp/README.md). */
#pragma once

#include <stdint.h>

#include <atomic>

namespace jlp {

/** Day / night classifier driven by SignalK `environment.mode`
 *  ("day" / "night"), as published by signalk-espos-manager from the
 *  Sun's altitude (@openwaters/almanac): day from sunrise to sunset.
 *  signalk-derived-data publishes the same path and is accepted too.
 *  Used by the idle dimmer so the panel only goes dark at night.
 *
 *  Fail-safe behaviour: until SK has delivered a value, classify as
 *  Day so the helm stays bright. Dimming while the operator needs the
 *  display is far worse than not saving power before the first delta.
 *
 *  A Signal K outage keeps the last mode: a short drop at night must not
 *  light the cabin. So an outage across sunrise keeps the night brightness
 *  until it ends or until espOS restarts the device (skLinkStalled after
 *  sk.stall_s, 5 min, with WiFi up; wifiStalled after 30 min without
 *  WiFi); a restart starts at Unknown, and Unknown is day.
 */
enum class DayNight : uint8_t { Unknown, Day, Night };

class SunState {
 public:
  /** Subscribe to environment.mode over the SK WS. Idempotent — safe
   *  to call once at boot only. */
  void hook_sk_ws();

  /** Best-effort current state. Returns Unknown until the first
   *  environment.mode delta arrives. */
  DayNight classify() const { return state_; }

  /** True once SK has delivered a usable environment.mode value.
   *  Mainly for diagnostics. */
  bool has_data() const { return state_ != DayNight::Unknown; }

  /** Setter used by the SK listener. Accepts "day" / "night"
   *  (case-insensitive); anything else leaves the state unchanged. */
  void set_mode(const char* mode);


 private:
  // Written on the Signal K stream task, read from the UI task.
  std::atomic<DayNight> state_{DayNight::Unknown};
};

SunState& sun_state();

}  // namespace jlp

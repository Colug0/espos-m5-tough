/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/* From espos-p4-cockpit main/jlp/zone_registry.cpp, changed for the M5Stack Tough (main/jlp/README.md). */
#include "zone_registry.h"

#include "esp_log.h"
#include "lvgl.h"

#include "subject_registry.h"

static const char* TAG = "jlp.zones";

namespace jlp {

namespace {

ZoneState parse_state(const char* s) {
  if (!s) return ZoneState::Normal;
  if (!strcmp(s, "nominal")) return ZoneState::Nominal;
  if (!strcmp(s, "normal")) return ZoneState::Normal;
  if (!strcmp(s, "alert")) return ZoneState::Alert;
  if (!strcmp(s, "warn")) return ZoneState::Warn;
  if (!strcmp(s, "alarm")) return ZoneState::Alarm;
  if (!strcmp(s, "emergency")) return ZoneState::Emergency;
  return ZoneState::Normal;
}

}  // namespace

uint32_t color_for_state(ZoneState s) {
  // Maritime-helm escalation palette: green safe → yellow notice → orange
  // act-soon → red act-now → purple critical/system-down. Bumps every SK
  // state one severity step warmer than the SK spec defaults so a glance
  // at the helm reads like a traffic light.
  switch (s) {
    case ZoneState::Nominal:
    case ZoneState::Normal:    return 0x3fb950;  // green
    case ZoneState::Alert:     return 0xd29922;  // yellow
    case ZoneState::Warn:      return 0xdb6d28;  // orange
    case ZoneState::Alarm:     return 0xf85149;  // red
    case ZoneState::Emergency: return 0xa371f7;  // purple
  }
  return 0xd29922;
}

const Zone* ZoneRegistry::match(const std::string& path,
                                float raw_value) const {
  auto it = map_.find(path);
  if (it == map_.end()) return nullptr;
  const auto& zs = it->second;
  // Find the zone with the highest upper bound — if the value sits
  // exactly on that upper edge (full tank reading 1.0 when zones are
  // [0..0.2 alarm, 0.2..1 normal]), accept it into that top zone.
  // Without this carve-out, "100%" on a ratio path falls out of every
  // zone (because [0.2, 1.0) excludes 1.0) and the widget renders
  // with the default fallback color instead of nominal.
  float top_edge = -1e30f;
  for (const Zone& z : zs) if (z.upper > top_edge) top_edge = z.upper;

  for (const Zone& z : zs) {
    // SK convention is half-open [lower, upper). That makes a "point
    // zone" where lower == upper (commonly authored for bool/int state
    // paths like a switch position — alert at 0, nominal at 1) match
    // nothing. Treat that case as equality on the point instead.
    if (z.lower == z.upper) {
      if (raw_value == z.lower) return &z;
    } else if (raw_value >= z.lower && raw_value < z.upper) {
      return &z;
    } else if (raw_value == z.upper && z.upper == top_edge) {
      // Boundary catch on the topmost zone only — keeps the rest of
      // the boundaries half-open per SK spec.
      return &z;
    }
  }
  return nullptr;
}

void ZoneRegistry::apply_meta(const std::string& path,
                              const JsonObjectConst& meta) {
  bool changed = false;
  const char* desc = meta["description"] | (const char*)nullptr;
  if (desc && *desc) {
    descriptions_[path] = desc;
    changed = true;
  }
  JsonArrayConst zarr = meta["zones"];
  // An empty array is Signal K removing the zones; keeping the old ones
  // would go on tinting the widget with a stale warning or alarm.
  if (!zarr.isNull() && zarr.size() == 0) {
    if (map_.erase(path) > 0) changed = true;
  } else if (!zarr.isNull()) {
    std::vector<Zone> zs;
    zs.reserve(zarr.size());
    for (JsonObjectConst z : zarr) {
      // SK convention: a missing `lower` means "-infinity" (the zone
      // covers everything below `upper`); a missing `upper` means
      // "+infinity". Single-sided zones are the common case for alarm
      // thresholds.
      zs.push_back({z["lower"] | -1e30f, z["upper"] | 1e30f,
                    parse_state(z["state"] | "normal")});
    }
    map_[path] = std::move(zs);
    changed = true;
  }
  // No per-path logging here — apply_meta fires once per bound path in
  // a burst right after every layout push (sendMeta=all rebroadcast),
  // and a remote-log write per line would stall the UI task.
  if (!changed) return;
  // Notify the bound subject so widget observers re-fire and pick
  // up the just-loaded description / zones. The observer reads
  // description() first and prefers it over the formatted value,
  // so a label built before the fetch ran swaps to its description
  // here, and zone-tinted bars pick up the zone bounds without
  // waiting for the next value delta. build_label also seeds the
  // label from zones().description(), so a label is legible even
  // if no value ever arrives.
  lv_subject_t* sub = registry().lookup(path);
  if (sub) lv_subject_notify(sub);
}

const std::string& ZoneRegistry::description(const std::string& path) const {
  static const std::string empty;
  auto it = descriptions_.find(path);
  return it == descriptions_.end() ? empty : it->second;
}

void ZoneRegistry::hook_sk_ws() {
  // Meta arrives per-path through SubjectRegistry's subscriptions, so
  // there is nothing to wire here; kept for the boot call site and the
  // WASM preview's stub.
  ESP_LOGI(TAG, "zone meta wired per-path via SubjectRegistry");
}

ZoneRegistry& zones() {
  static ZoneRegistry r;
  return r;
}

}  // namespace jlp

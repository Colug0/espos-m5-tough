/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/* From espos-p4-cockpit main/jlp/idle_dimmer.cpp, changed for the M5Stack Tough (main/jlp/README.md). */
#include "idle_dimmer.h"

#include "esp_log.h"
#include "lvgl.h"

#include "tough_hal/ui.h"
#include "subject_registry.h"
#include "sun_state.h"
#include "wake_overlay.h"

static const char* TAG = "jlp.dimmer";

namespace jlp {

void IdleDimmer::set_on(bool on) {
  if (on == on_) return;
  on_ = on;
  auto* d = tough_hal::ui::display();
  if (!d) return;
  if (on) {
    // Hide the wake overlay BEFORE raising the backlight so the user
    // doesn't see the layout content flash up under the overlay.
    wake_overlay().hide();
    shown_pct_ = on_brightness();
    d->set_brightness(shown_pct_);
  } else {
    // Show the "tap to wake" overlay BEFORE killing the backlight so
    // the panel doesn't briefly show the layout before going dark,
    // and so the first wake-tap can't fall through to a toggle or
    // button underneath.
    wake_overlay().show();
    d->set_brightness(dim_pct_);
  }
  ESP_LOGI(TAG, "backlight %s (pct=%u)", on ? "on" : "off",
           (unsigned)(on ? on_brightness() : dim_pct_));
}

bool IdleDimmer::night_active() const {
  return night_pct_ > 0 && sun_state().classify() == DayNight::Night;
}

uint8_t IdleDimmer::on_brightness() const {
  return night_active() ? night_pct_ : day_pct_;
}

void IdleDimmer::set_day_brightness(uint8_t pct) {
  day_pct_ = pct;
  refresh();
}

void IdleDimmer::set_night_brightness(uint8_t pct) {
  night_pct_ = pct;
  refresh();
}

void IdleDimmer::refresh() {
  const uint8_t level = on_brightness();
  if (level == shown_pct_) return;
  shown_pct_ = level;
  // While dimmed the dim level is what belongs on screen; the new level
  // takes effect on the next wake.
  if (on_) {
    if (auto* d = tough_hal::ui::display()) d->set_brightness(level);
  }
  // The slider shows the level that applies now, so dragging it at night
  // adjusts the night brightness.
  if (lv_subject_t* s = registry().lookup("@brightness")) {
    lv_subject_set_float(s, (float)level);
  }
  ESP_LOGI(TAG, "brightness %u%% (%s)", (unsigned)level,
           night_active() ? "night" : "day");
}

void IdleDimmer::init() {
  // LVGL tracks input inactivity automatically. Poll on a 1 Hz cadence
  // — finer resolution is wasteful for human-scale timeouts.
  tough_hal::ui::every(1000, [this]() {
    // Day and night switch here too, within a second of environment.mode.
    refresh();
    if (idle_timeout_sec_ == 0) {
      if (!on_) set_on(true);
      return;
    }
    // Day/night gating: only let the panel dim during night. Day
    // (or Unknown, e.g. SK hasn't published sunrise/sunset yet)
    // is fail-safe-bright. Sun classification is refreshed cheap
    // on every tick — classify() is two integer comparisons.
    if (sun_state().classify() != DayNight::Night) {
      if (!on_) set_on(true);
      return;
    }
    // LVGL 9.6 deprecates NULL for "the default display".
    uint32_t inactive_ms = lv_display_get_inactive_time(lv_display_get_default());
    bool should_be_on = inactive_ms < idle_timeout_sec_ * 1000U;
    if (should_be_on != on_) set_on(should_be_on);
  });
}

void IdleDimmer::configure(uint32_t idle_timeout_sec, uint8_t dim_pct) {
  if (dim_pct > 100) dim_pct = 100;
  if (idle_timeout_sec_ != idle_timeout_sec || dim_pct_ != dim_pct) {
    idle_timeout_sec_ = idle_timeout_sec;
    dim_pct_ = dim_pct;
    ESP_LOGI(TAG, "idle timeout=%us dim_pct=%u",
             (unsigned)idle_timeout_sec_, (unsigned)dim_pct_);
  }
  // Always re-arm + wake regardless of whether the config actually
  // changed. A fresh layout push means the operator is at the panel
  // right now, even if the values match what was already loaded.
  wake();
}

void IdleDimmer::wake() {
  lv_display_trigger_activity(lv_display_get_default());
  if (!on_) set_on(true);
}

IdleDimmer& idle_dimmer() {
  static IdleDimmer d;
  return d;
}

}  // namespace jlp

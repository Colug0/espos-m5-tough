/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/* From espos-p4-cockpit main/jlp/wake_overlay.h, changed for the M5Stack Tough (main/jlp/README.md). */
#pragma once

#include "lvgl.h"

namespace jlp {

/** Full-screen "tap to wake" overlay shown while the backlight is in
 *  the idle-dim state. Sits above all widgets so the first tap that
 *  wakes the panel can't accidentally trigger a toggle or button
 *  underneath. Stays below the alert overlay so notifications still
 *  pop on top.
 *
 *  Created once at boot. Idle dimmer calls show() / hide() as it
 *  transitions on/off. */
class WakeOverlay {
 public:
  void init();
  void show();
  void hide();
  bool is_visible() const { return visible_; }

 private:
  lv_obj_t* root_ = nullptr;
  bool visible_ = false;
};

WakeOverlay& wake_overlay();

}  // namespace jlp

/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/* From espos-p4-cockpit main/jlp/status_overlay.h, changed for the M5Stack Tough (main/jlp/README.md). */
#pragma once

#include "lvgl.h"
#include <stdint.h>

namespace jlp {

// A small status strip pinned to the top of the active screen. Survives
// every layout swap — never created from user-supplied JSON. The
// remaining area of the screen is `content_root()` and is where the
// layout player parents its widget tree.
class StatusOverlay {
 public:
  void init();

  // Setters; safe to call from the UI task.
  void set_hostname(const char* hostname);
  void set_wifi(const char* line);                   // "MOIN -58dBm" or "down"
  void set_sk(const char* line);                     // "ok" / "connecting" ...
  void set_n2k(int64_t rx_idle_seconds, unsigned clients);
  void set_uptime_heap(uint32_t uptime_s, uint32_t free_heap);

  // Record the configured SK server so the connection-lost banner can
  // name it. Call once at boot with the server espOS selected
  // (espos_sk_get_server).
  void set_sk_server(const char* host, uint16_t port);

  // Show / hide a prominent full-width "connection lost" banner. Lives
  // at screen level (NOT inside the status strip), so it stays visible
  // even when a layout disables the strip via `status_overlay: false`.
  // Driven from the SK WS connection-state consumer: shown on
  // Disconnected, hidden on Connected.
  void show_sk_lost();
  void hide_sk_lost();

  lv_obj_t* content_root() { return content_root_; }

  // Show or hide the strip. When hidden, content_root() expands to
  // fill the full screen. Re-show restores the strip and shrinks
  // content_root by kStripHeight from the top.
  void set_visible(bool visible);
  bool is_visible() const { return visible_; }

  // While a layout hides the strip, a swipe down from the top edge shows
  // it above the content for this long; any touch while it shows starts
  // the time again, a tap on it closes it.
  void set_peek_seconds(uint32_t seconds);

  // While the Signal K stream is down (no network counts), the strip
  // shows above the content even when the layout hides it: the operator
  // sees why values stopped.
  void set_fault(bool fault);

 private:
  void on_press();
  void track_swipe();
  void update_float();

  lv_obj_t* strip_ = nullptr;
  lv_obj_t* content_root_ = nullptr;
  lv_obj_t* lbl_host_ = nullptr;
  lv_obj_t* lbl_wifi_ = nullptr;
  lv_obj_t* lbl_sk_ = nullptr;
  lv_obj_t* lbl_n2k_ = nullptr;
  lv_obj_t* lbl_sys_ = nullptr;
  lv_obj_t* sk_lost_ = nullptr;       // screen-level connection-lost banner
  lv_obj_t* sk_lost_lbl_ = nullptr;   // its text
  char sk_server_[40] = "";           // "host:port" for the banner text
  bool visible_ = true;
  bool fault_ = true;           // no Signal K stream until the first connect
  bool peeking_ = false;
  uint32_t peek_ms_ = 5000;
  uint32_t peek_start_ = 0;     // lv_tick of the swipe or the last touch
  lv_indev_t* indev_ = nullptr;
  lv_point_t press_{};          // where the current press started
  uint32_t swipe_timer_ = 0;    // ui::every handle while a swipe is tracked
};

StatusOverlay& overlay();

}  // namespace jlp

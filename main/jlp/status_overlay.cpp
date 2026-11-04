/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/* From espos-p4-cockpit main/jlp/status_overlay.cpp, changed for the M5Stack Tough (main/jlp/README.md). */
#include "status_overlay.h"

#include <cstdio>

#include "tough_hal/ui.h"

namespace jlp {

namespace {
constexpr int kStripHeight = 28;
// A swipe starts this close to the top edge and travels this far down,
// more down than sideways. The band is a little wider than the strip:
// a finger coming off the bezel lands a few pixels in.
constexpr int32_t kEdgeBand = 24;
constexpr int32_t kSwipeMin = 30;
constexpr uint32_t kBgHex = 0x161b22;
constexpr uint32_t kFgHex = 0xe6edf3;
constexpr uint32_t kMutedHex = 0x8b949e;

lv_obj_t* make_label(lv_obj_t* parent, const char* init) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_color(l, lv_color_hex(kFgHex), LV_PART_MAIN);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(l, init);
  return l;
}
}  // namespace

void StatusOverlay::init() {
  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d1117), LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

  // The rest of the screen, where layouts live. Created before the strip
  // so the strip paints above it when it floats over a full-height layout.
  content_root_ = lv_obj_create(scr);
  lv_obj_set_size(content_root_, LV_HOR_RES, LV_VER_RES - kStripHeight);
  lv_obj_align(content_root_, LV_ALIGN_TOP_MID, 0, kStripHeight);
  lv_obj_set_style_bg_color(content_root_, lv_color_hex(0x0d1117),
                            LV_PART_MAIN);
  lv_obj_set_style_border_width(content_root_, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(content_root_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(content_root_, 0, LV_PART_MAIN);
  lv_obj_set_scrollable(content_root_, false);

  strip_ = lv_obj_create(scr);
  lv_obj_set_size(strip_, LV_HOR_RES, kStripHeight);
  lv_obj_align(strip_, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(strip_, lv_color_hex(kBgHex), LV_PART_MAIN);
  lv_obj_set_style_border_width(strip_, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(strip_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(strip_, 4, LV_PART_MAIN);
  lv_obj_set_flex_flow(strip_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(strip_, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollable(strip_, false);

  lbl_host_ = make_label(strip_, "host:?");
  lbl_wifi_ = make_label(strip_, "wifi:?");
  lbl_sk_ = make_label(strip_, "sk:?");
  // A 320 px strip holds three short labels at 14 px; the N2K and
  // uptime/heap labels only fit a wider panel. Their setters skip a label
  // that was not created.
  if (LV_HOR_RES >= 480) {
    lbl_n2k_ = make_label(strip_, "n2k:?");
    lbl_sys_ = make_label(strip_, "up 0s heap ?");
  }

  // A tap on a floating strip closes it; a pinned one ignores taps.
  lv_obj_set_clickable(strip_, true);
  lv_obj_add_event_cb(
      strip_,
      [](lv_event_t* e) {
        auto* self = static_cast<StatusOverlay*>(lv_event_get_user_data(e));
        self->peeking_ = false;
        self->update_float();
      },
      LV_EVENT_CLICKED, this);

  // Presses are watched on the input device, not on an object: the swipe
  // may start on any widget of the layout.
  indev_ = lv_indev_get_next(nullptr);
  if (indev_ != nullptr) {
    lv_indev_add_event_cb(
        indev_,
        [](lv_event_t* e) {
          static_cast<StatusOverlay*>(lv_event_get_user_data(e))->on_press();
        },
        LV_EVENT_PRESSED, this);
  }
  tough_hal::ui::every(250, [this]() { update_float(); });

  // Connection-lost banner: a full-width red bar pinned to the bottom,
  // parented to the SCREEN (not the strip) so it shows even when a
  // layout sets status_overlay:false and hides the strip. Hidden until
  // the SK WS reports Disconnected. Created last so it paints above the
  // content; alert_overlay / wake_overlay move_foreground after us, so
  // a full-screen alarm still covers it.
  sk_lost_ = lv_obj_create(scr);
  lv_obj_set_size(sk_lost_, LV_HOR_RES, 40);
  lv_obj_align(sk_lost_, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(sk_lost_, lv_color_hex(0xf85149), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sk_lost_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(sk_lost_, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(sk_lost_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(sk_lost_, 4, LV_PART_MAIN);
  lv_obj_set_scrollable(sk_lost_, false);
  lv_obj_set_hidden(sk_lost_, true);
  sk_lost_lbl_ = lv_label_create(sk_lost_);
  lv_obj_set_style_text_color(sk_lost_lbl_, lv_color_hex(0xffffff),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(sk_lost_lbl_, &lv_font_montserrat_14,
                             LV_PART_MAIN);
  lv_label_set_text(sk_lost_lbl_, "Signal K connection lost");
  lv_obj_center(sk_lost_lbl_);
}

void StatusOverlay::set_hostname(const char* hostname) {
  if (!lbl_host_) return;
  lv_label_set_text(lbl_host_, hostname ? hostname : "host:?");
}

void StatusOverlay::set_wifi(const char* line) {
  if (!lbl_wifi_) return;
  lv_label_set_text_fmt(lbl_wifi_, "wifi:%s", line ? line : "?");
}

void StatusOverlay::set_sk(const char* line) {
  if (!lbl_sk_) return;
  lv_label_set_text_fmt(lbl_sk_, "sk:%s", line ? line : "?");
}

void StatusOverlay::set_sk_server(const char* host, uint16_t port) {
  snprintf(sk_server_, sizeof(sk_server_), "%s:%u", host ? host : "?",
           (unsigned)port);
}

void StatusOverlay::show_sk_lost() {
  if (!sk_lost_ || !sk_lost_lbl_) return;
  if (sk_server_[0]) {
    lv_label_set_text_fmt(sk_lost_lbl_,
                          "Signal K connection lost - %s", sk_server_);
  } else {
    lv_label_set_text(sk_lost_lbl_, "Signal K connection lost");
  }
  // Keep above the live content even after layout swaps reparent it.
  lv_obj_move_foreground(sk_lost_);
  lv_obj_set_hidden(sk_lost_, false);
}

void StatusOverlay::hide_sk_lost() {
  if (!sk_lost_) return;
  lv_obj_set_hidden(sk_lost_, true);
}

void StatusOverlay::set_n2k(int64_t rx_idle_seconds, unsigned clients) {
  if (!lbl_n2k_) return;
  if (rx_idle_seconds < 0) {
    lv_label_set_text_fmt(lbl_n2k_, "n2k:- cl:%u", clients);
  } else {
    lv_label_set_text_fmt(lbl_n2k_, "n2k:%llds cl:%u",
                          (long long)rx_idle_seconds, clients);
  }
}

void StatusOverlay::set_uptime_heap(uint32_t uptime_s, uint32_t free_heap) {
  if (!lbl_sys_) return;
  lv_label_set_text_fmt(lbl_sys_, "up %us heap %uK", (unsigned)uptime_s,
                        (unsigned)(free_heap / 1024));
}

void StatusOverlay::set_visible(bool visible) {
  if (visible == visible_) return;
  visible_ = visible;
  peeking_ = false;
  if (!strip_ || !content_root_) return;
  if (visible) {
    lv_obj_set_hidden(strip_, false);
    lv_obj_set_size(content_root_, LV_HOR_RES, LV_VER_RES - kStripHeight);
    lv_obj_align(content_root_, LV_ALIGN_TOP_MID, 0, kStripHeight);
  } else {
    lv_obj_set_size(content_root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(content_root_, LV_ALIGN_TOP_MID, 0, 0);
    update_float();
  }
}

void StatusOverlay::set_peek_seconds(uint32_t seconds) {
  peek_ms_ = seconds * 1000u;
}

void StatusOverlay::set_fault(bool fault) {
  fault_ = fault;
  update_float();
}

void StatusOverlay::on_press() {
  if (peeking_) peek_start_ = lv_tick_get();  // still reading it
  if (visible_ || swipe_timer_ != 0 || indev_ == nullptr) return;
  lv_indev_get_point(indev_, &press_);
  if (press_.y >= kEdgeBand) return;
  // PRESSING reaches objects only, so the movement is sampled here.
  swipe_timer_ = tough_hal::ui::every(20, [this]() { track_swipe(); });
}

void StatusOverlay::track_swipe() {
  bool done = lv_indev_get_state(indev_) != LV_INDEV_STATE_PRESSED;
  if (!done) {
    lv_point_t p{};
    lv_indev_get_point(indev_, &p);
    const int32_t dy = p.y - press_.y;
    const int32_t dx = LV_ABS(p.x - press_.x);
    if (dy >= kSwipeMin && dy > dx) {
      // The widget under the finger gets PRESS_LOST instead of a click.
      lv_indev_wait_release(indev_);
      peeking_ = true;
      peek_start_ = lv_tick_get();
      update_float();
      done = true;
    }
  }
  if (done) {
    tough_hal::ui::cancel(swipe_timer_);
    swipe_timer_ = 0;
  }
}

// Only for a strip the layout hides: show it above the content while a
// peek lasts or the stream is down.
void StatusOverlay::update_float() {
  if (visible_ || strip_ == nullptr) return;
  if (peeking_ && lv_tick_elaps(peek_start_) >= peek_ms_) peeking_ = false;
  lv_obj_set_hidden(strip_, !(peeking_ || fault_));
}

StatusOverlay& overlay() {
  static StatusOverlay s;
  return s;
}

}  // namespace jlp

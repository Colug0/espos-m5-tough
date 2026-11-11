/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/* From espos-p4-cockpit main/jlp/layout/layout_manager.cpp, changed for the M5Stack Tough (main/jlp/README.md). */
#include "layout_manager.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include "esp_log.h"
#include "tough_hal/ui.h"

#include "../alert_overlay.h"
#include "../idle_dimmer.h"
#include "../notifications_registry.h"
#include "../status_overlay.h"
#include "../subject_registry.h"
#include "../widgets/widget_factory.h"
#include "store.h"

static const char* TAG = "jlp.layout";

namespace jlp {

namespace {

constexpr int kSchemaVersion = 1;
constexpr size_t kMaxJsonBytes = 64 * 1024;

// Per-screen container + tab button colors.
constexpr uint32_t kStripBgHex = 0x161b22;
constexpr uint32_t kTabBgHex = 0x21262d;
constexpr uint32_t kTabActiveBgHex = 0x58a6ff;
constexpr uint32_t kTabFgHex = 0xe6edf3;
constexpr uint32_t kTabActiveFgHex = 0x0d1117;

// A page swipe travels this far, mostly sideways. A swipe up for the
// overlay strip starts this close to the bottom edge and travels the
// second distance; the band is a little wider than a finger coming off
// the bezel needs.
constexpr int32_t kPageSwipeMin = 40;
constexpr int32_t kEdgeBand = 24;
constexpr int32_t kEdgeSwipeMin = 30;

struct ScreenSwitcherCtx {
  std::vector<lv_obj_t*> screens;
  std::vector<lv_obj_t*> tabs;
  // Screen ids, parallel to `screens`. Index is not a stable handle
  // across layout edits, so remote selection addresses screens by id.
  std::vector<std::string> ids;
  int active = 0;
  lv_obj_t* strip = nullptr;
  // tab_strip "overlay-bottom": the strip floats above the page, hidden
  // except while it peeks.
  bool overlay = false;
  bool peeking = false;
  uint32_t peek_start = 0;
  // Page groups resolved against `ids`, indexed like the configured
  // groups: the page indices each shows, empty for a group this layout
  // has none of. Empty when ungrouped.
  std::vector<std::vector<int>> members;
  std::vector<int> last;  // per group, the page shown last in it, -1 none
  int group = -1;         // the group shown, -1 ungrouped
  lv_obj_t* group_tab = nullptr;
};

static bool contains(const std::vector<int>& v, int i) {
  return std::find(v.begin(), v.end(), i) != v.end();
}

static bool visible_in_group(const ScreenSwitcherCtx* ctx, int i) {
  return ctx->group < 0 || contains(ctx->members[ctx->group], i);
}

// The next group after `from` that this layout can show; `from` itself
// when it is the only one, -1 when there is none.
static int next_usable_group(const ScreenSwitcherCtx* ctx, int from) {
  const int n = (int)ctx->members.size();
  for (int k = 1; k <= n; k++) {
    const int g = ((from < 0 ? -1 : from) + k + n) % n;
    if (!ctx->members[g].empty()) return g;
  }
  return -1;
}

// One per layout. Lives in heap, freed when the layout's root is
// destroyed (event_cb on root).
static void switcher_show(ScreenSwitcherCtx* ctx, int idx) {
  if (idx < 0 || idx >= (int)ctx->screens.size()) return;
  for (size_t i = 0; i < ctx->screens.size(); i++) {
    lv_obj_set_hidden(ctx->screens[i], (int)i != idx);
  }
  for (size_t i = 0; i < ctx->tabs.size(); i++) {
    bool a = (int)i == idx;
    lv_obj_set_style_bg_color(
        ctx->tabs[i], lv_color_hex(a ? kTabActiveBgHex : kTabBgHex),
        LV_PART_MAIN);
    lv_obj_t* lbl = lv_obj_get_child(ctx->tabs[i], 0);
    if (lbl) {
      lv_obj_set_style_text_color(
          lbl, lv_color_hex(a ? kTabActiveFgHex : kTabFgHex), LV_PART_MAIN);
    }
  }
  ctx->active = idx;
  if (ctx->group >= 0) ctx->last[ctx->group] = idx;
  // Stream widgets open/close their network stream with screen visibility —
  // a hidden screen must not keep pulling frames through the hosted link.
  for (size_t i = 0; i < ctx->screens.size(); i++) {
    stream_widgets_notify_visibility(ctx->screens[i], (int)i == idx);
  }
}

// Hides the tabs of pages outside the shown group; the flex row spreads
// the rest. The group tab names the group a tap brings.
static void show_group_tabs(ScreenSwitcherCtx* ctx,
                            const std::vector<ScreenGroup>& groups) {
  for (size_t i = 0; i < ctx->tabs.size(); i++) {
    lv_obj_set_hidden(ctx->tabs[i], !visible_in_group(ctx, (int)i));
  }
  if (ctx->group_tab == nullptr) return;
  const int next = ctx->group < 0 ? -1 : next_usable_group(ctx, ctx->group);
  const bool shown = next >= 0 && next != ctx->group &&
                     next < (int)groups.size();
  lv_obj_set_hidden(ctx->group_tab, !shown);
  if (shown) {
    lv_label_set_text(lv_obj_get_child(ctx->group_tab, 0),
                      groups[next].name.c_str());
  }
}

// Shows or hides an overlay strip; a fixed strip ignores it.
static void strip_peek(ScreenSwitcherCtx* ctx, bool on) {
  if (!ctx->overlay || ctx->strip == nullptr) return;
  ctx->peeking = on;
  if (on) ctx->peek_start = lv_tick_get();
  lv_obj_set_hidden(ctx->strip, !on);
}

// Builds the screen(s) under `root`. Single screen: widgets parented
// directly to root. Multi-screen: each screen gets its own container
// stacked top-to-top, only the active one is visible, and a bottom
// strip of tab buttons switches between them.
bool build_screens(lv_obj_t* root, JsonObjectConst doc, JsonArrayConst screens,
                   SubjectRegistry& reg, std::string* err,
                   std::set<std::string>* live_paths, unsigned* widget_count) {
  if (screens.size() == 0) { *err = "no screens"; return false; }

  if (screens.size() == 1) {
    JsonObjectConst s = screens[0];
    JsonArrayConst widgets = s["widgets"];
    BuildCtx ctx{root, reg, *live_paths};
    for (JsonObjectConst w : widgets) {
      if (!build_widget(ctx, w, err)) return false;
      ++*widget_count;
    }
    return true;
  }

  // Multi-screen: stack of hidden containers + bottom tab strip.
  const int strip_h = doc["tab_strip_height"] | 56;
  auto* sctx = new ScreenSwitcherCtx();
  // An unknown mode is drawn fixed: a newer client's value must not
  // hide the tabs on a device that cannot bring them back.
  sctx->overlay =
      strcmp(doc["tab_strip"] | kTabStripFixed, kTabStripOverlayBottom) == 0;

  // Free the switcher context when the layout root dies.
  lv_obj_set_user_data(root, sctx);
  lv_obj_add_event_cb(
      root,
      [](lv_event_t* e) {
        delete static_cast<ScreenSwitcherCtx*>(lv_obj_get_user_data(
            static_cast<lv_obj_t*>(lv_event_get_target(e))));
      },
      LV_EVENT_DELETE, nullptr);

  // `root` is a column: the visible page fills what the strip leaves, the
  // strip sits at the bottom. Sized from root's real height, which is the
  // display minus the status strip when that is shown. A page sized
  // LV_VER_RES - strip_h overlapped the tabs by the status strip's 28 px and,
  // being above them in z-order, took their taps (on a 240 px panel, half of
  // each tab). Hidden pages leave the flex flow. An overlay strip is out
  // of the flow: the pages take all of root and the strip floats over
  // their bottom.
  if (!sctx->overlay) {
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 0, LV_PART_MAIN);
  }

  lv_obj_t* strip = lv_obj_create(root);
  lv_obj_set_size(strip, lv_pct(100), strip_h);
  lv_obj_set_style_bg_color(strip, lv_color_hex(kStripBgHex), LV_PART_MAIN);
  lv_obj_set_style_border_width(strip, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(strip, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(strip, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_gap(strip, 4, LV_PART_MAIN);
  lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollable(strip, false);
  if (sctx->overlay) {
    lv_obj_align(strip, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_hidden(strip, true);
  }
  sctx->strip = strip;

  // Per-screen container above the strip, all sharing the same area.
  for (JsonObjectConst s : screens) {
    lv_obj_t* page = lv_obj_create(root);
    lv_obj_set_width(page, lv_pct(100));
    if (sctx->overlay) {
      lv_obj_set_height(page, lv_pct(100));
    } else {
      lv_obj_set_flex_grow(page, 1);
    }
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(page, false);
    sctx->screens.push_back(page);
    sctx->ids.push_back(s["id"] | "");

    JsonArrayConst widgets = s["widgets"];
    BuildCtx ctx{page, reg, *live_paths};
    for (JsonObjectConst w : widgets) {
      if (!build_widget(ctx, w, err)) return false;
      ++*widget_count;
    }
  }
  // Last child: below the pages in the column, or painted above them
  // as an overlay.
  lv_obj_move_to_index(strip, -1);

  // Tab buttons (one per screen, flex-row equal widths).
  for (size_t i = 0; i < screens.size(); i++) {
    JsonObjectConst s = screens[i];
    const char* title = s["title"] | (s["id"] | "?");

    lv_obj_t* btn = lv_obj_create(strip);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, lv_pct(100));
    lv_obj_set_style_bg_color(btn, lv_color_hex(kTabBgHex), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(btn, false);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kTabFgHex), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(lbl);

    // Stash the screen index as a small int in user_data.
    lv_obj_set_user_data(btn, (void*)(intptr_t)i);
    lv_obj_add_event_cb(
        btn,
        [](lv_event_t* e) {
          auto* w = static_cast<lv_obj_t*>(lv_event_get_target(e));
          auto* parent_root = lv_obj_get_parent(lv_obj_get_parent(w));
          auto* ctx =
              static_cast<ScreenSwitcherCtx*>(lv_obj_get_user_data(parent_root));
          int idx = (int)(intptr_t)lv_obj_get_user_data(w);
          if (ctx) {
            switcher_show(ctx, idx);
            strip_peek(ctx, false);  // the choice is made
          }
        },
        LV_EVENT_CLICKED, nullptr);

    sctx->tabs.push_back(btn);
  }

  // The group tab, last in the row and as wide as its label up to a third
  // of the strip, so a long group name cannot squeeze the page tabs; a
  // longer name ends in dots. Hidden until page groups give it a group to
  // switch to.
  {
    lv_obj_t* btn = lv_obj_create(strip);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, lv_pct(100));
    lv_obj_set_style_max_width(btn, lv_pct(33), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kStripBgHex), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(kTabActiveBgHex),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(btn, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(btn, false);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "");
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(lbl, lv_pct(100), LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kTabActiveBgHex),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(
        btn, [](lv_event_t*) { layout_manager().next_group(); },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_set_hidden(btn, true);
    sctx->group_tab = btn;
  }

  switcher_show(sctx, 0);
  return true;
}

// Screen selection by id, for the tab strip's remote equivalent.
// Both must run on the UI task: they touch LVGL and read the ctx that
// the layout root owns. A single-screen layout has no switcher at all
// (build_screens() never allocates one), so user_data is NULL there —
// hence the guard rather than a bare cast.
static ScreenSwitcherCtx* switcher_of(lv_obj_t* root) {
  if (!root) return nullptr;
  return static_cast<ScreenSwitcherCtx*>(lv_obj_get_user_data(root));
}

// Subject kind a given widget type expects on its `bind` path. Must
// stay in lockstep with the get_or_create() calls in widget_factory.cpp.
// Returns nullopt for widgets that don't bind a path at all (button,
// notifications) — those skip the kind-conflict check.
std::optional<SubjectKind> expected_subject_kind(const char* type) {
  if (!type) return std::nullopt;
  if (!strcmp(type, "label"))    return SubjectKind::Float;
  if (!strcmp(type, "value"))    return SubjectKind::Float;
  if (!strcmp(type, "arc"))      return SubjectKind::Float;
  if (!strcmp(type, "bar"))      return SubjectKind::Float;
  if (!strcmp(type, "toggle"))   return SubjectKind::Int;
  // bargroup: handled separately because each sub-bar is its own
  // (path, kind) pair. button + notifications: no bind. Anything
  // unknown returns nullopt and is left to validate's existing
  // dispatch error path.
  return std::nullopt;
}

const char* subject_kind_name(SubjectKind k) {
  switch (k) {
    case SubjectKind::Float:  return "Float";
    case SubjectKind::Int:    return "Int";
    case SubjectKind::Bool:   return "Bool";
    case SubjectKind::String: return "String";
  }
  return "?";
}

// Record a (path, kind) requirement into `seen`. Returns false +
// fills *err on conflict (path already requested with a different
// kind).
bool record_path_kind(const std::string& path, SubjectKind kind,
                      std::unordered_map<std::string, SubjectKind>* seen,
                      const std::string& widget_label, std::string* err) {
  auto it = seen->find(path);
  if (it == seen->end()) {
    (*seen)[path] = kind;
    return true;
  }
  if (it->second != kind) {
    *err = std::string("kind conflict on bind path '") + path +
           "': " + widget_label + " wants " + subject_kind_name(kind) +
           " but a previous widget already bound it as " +
           subject_kind_name(it->second);
    return false;
  }
  return true;
}

// Validate global structure before building. Catches duplicates,
// missing required fields, AND kind conflicts on shared bind paths
// (e.g. one widget wants a path as Float, another as Int) so the
// layout is rejected up-front rather than half-built in LVGL.
bool validate(JsonObjectConst doc, std::string* err) {
  int schema = doc["schema"] | 0;
  if (schema != kSchemaVersion) {
    char buf[64];
    snprintf(buf, sizeof(buf), "unsupported schema %d (want %d)", schema,
             kSchemaVersion);
    *err = buf;
    return false;
  }
  JsonArrayConst screens = doc["screens"];
  if (screens.isNull() || screens.size() == 0) {
    *err = "no screens";
    return false;
  }

  std::unordered_set<std::string> ids;
  // path -> kind that's already been requested across the whole
  // layout. Survives screens, since widgets on different screens can
  // still bind the same path.
  std::unordered_map<std::string, SubjectKind> seen_paths;
  for (JsonObjectConst s : screens) {
    const char* sid = s["id"] | (const char*)nullptr;
    if (!sid) { *err = "screen missing id"; return false; }
    if (!ids.insert(sid).second) {
      *err = std::string("duplicate screen id: ") + sid;
      return false;
    }
    JsonArrayConst widgets = s["widgets"];
    std::unordered_set<std::string> wids;
    for (JsonObjectConst w : widgets) {
      const char* wid = w["id"] | (const char*)nullptr;
      if (!wid) { *err = "widget missing id"; return false; }
      if (!wids.insert(wid).second) {
        *err = std::string("duplicate widget id in screen ") + sid + ": " + wid;
        return false;
      }
      const char* type = w["type"] | (const char*)nullptr;
      if (!type) {
        *err = std::string("widget missing type in screen ") + sid;
        return false;
      }

      // Kind-conflict check. bargroup is a bag of independent sub-
      // bars; everything else has at most one bind path.
      const std::string label = std::string(type) + " '" + wid + "'";
      if (!strcmp(type, "bargroup")) {
        JsonArrayConst bars = w["bars"];
        for (JsonObjectConst b : bars) {
          const char* bp = b["bind"] | (const char*)nullptr;
          if (!bp) continue;  // sub-bar with no bind is a render-only stub
          if (!record_path_kind(bp, SubjectKind::Float, &seen_paths,
                                label, err)) {
            return false;
          }
        }
        continue;
      }
      const char* bind = w["bind"] | (const char*)nullptr;
      if (!bind) continue;  // toggle requires bind, others don't —
                            // missing-required-bind is build_*'s job
      auto kind = expected_subject_kind(type);
      if (!kind) continue;  // unknown / no-bind kinds: leave to dispatch
      if (!record_path_kind(bind, *kind, &seen_paths, label, err)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

// The press landed on the layout, not on an overlay above it (alert,
// wake, status strip).
static bool inside(lv_obj_t* obj, lv_obj_t* root) {
  for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
    if (o == root) return true;
  }
  return false;
}

// A drag on a slider or arc moves it; it must not also turn the page.
static bool drags(lv_obj_t* obj, lv_obj_t* root) {
  for (lv_obj_t* o = obj; o != nullptr && o != root; o = lv_obj_get_parent(o)) {
    if (lv_obj_check_type(o, &lv_slider_class) ||
        lv_obj_check_type(o, &lv_arc_class)) {
      return true;
    }
  }
  return false;
}

void LayoutManager::init(lv_obj_t* parent) {
  parent_ = parent;
  // Presses are watched on the input device, not on an object: a swipe
  // may start on any widget of the page.
  indev_ = lv_indev_get_next(nullptr);
  if (indev_ != nullptr) {
    lv_indev_add_event_cb(
        indev_,
        [](lv_event_t* e) {
          static_cast<LayoutManager*>(lv_event_get_user_data(e))->on_press();
        },
        LV_EVENT_PRESSED, this);
  }
  tough_hal::ui::every(250, [this]() {
    ScreenSwitcherCtx* ctx = switcher_of(current_root_);
    if (ctx != nullptr && ctx->peeking &&
        lv_tick_elaps(ctx->peek_start) >= tab_peek_ms_) {
      strip_peek(ctx, false);
    }
  });
}

void LayoutManager::on_press() {
  lv_indev_get_point(indev_, &press_);
  lv_obj_t* hit = lv_indev_search_obj(lv_screen_active(), &press_);
  // Where the press began decides for select_screen(): the widget keeps
  // the press (PRESS_LOCK) even when the finger moves onto an overlay.
  press_on_layout_ = inside(hit, current_root_);
  // Called from the PRESSED event, while LVGL has the pressed object set.
  pressed_ = lv_indev_get_active_obj();
  ScreenSwitcherCtx* ctx = switcher_of(current_root_);
  if (ctx == nullptr || swipe_timer_ != 0) return;
  if (ctx->peeking) ctx->peek_start = lv_tick_get();  // still choosing
  if (!press_on_layout_ || drags(hit, current_root_)) return;
  press_at_edge_ = ctx->overlay && !ctx->peeking &&
                   press_.y >= LV_VER_RES - kEdgeBand;
  // PRESSING reaches objects only, so the movement is sampled here.
  swipe_timer_ = tough_hal::ui::every(20, [this]() { track_swipe(); });
}

void LayoutManager::cancel_swipe() {
  if (swipe_timer_ == 0) return;
  tough_hal::ui::cancel(swipe_timer_);
  swipe_timer_ = 0;
}

// Ends the current press now. lv_indev_wait_release alone sends the widget
// PRESS_LOST only when the finger lifts, so a button that has sent its
// press value would hold it while its page is already gone. The widget the
// press began on gets PRESS_LOST at once instead, before a page change
// hides it; on the release LVGL sends a second one, which a button that has
// already released ignores. It is the object LVGL holds as pressed, captured
// in on_press, since a press-locked widget keeps the press wherever the
// finger moves; lv_obj_is_valid covers one a layout swap has deleted. A
// slider or an arc is left to LVGL: their handlers expect a live input
// device.
void LayoutManager::end_press() {
  lv_obj_t* obj = pressed_;
  if (obj != nullptr && lv_obj_is_valid(obj) && inside(obj, current_root_) &&
      !drags(obj, current_root_)) {
    lv_obj_send_event(obj, LV_EVENT_PRESS_LOST, indev_);
  }
  lv_indev_wait_release(indev_);
}

void LayoutManager::track_swipe() {
  ScreenSwitcherCtx* ctx = switcher_of(current_root_);
  // A scrolling list keeps its own drag.
  bool done = ctx == nullptr ||
              lv_indev_get_state(indev_) != LV_INDEV_STATE_PRESSED ||
              lv_indev_get_scroll_obj(indev_) != nullptr;
  if (!done) {
    lv_point_t p{};
    lv_indev_get_point(indev_, &p);
    const int32_t dx = p.x - press_.x;
    const int32_t dy = p.y - press_.y;
    if (LV_ABS(dx) >= kPageSwipeMin && LV_ABS(dx) > 2 * LV_ABS(dy)) {
      // The widget under the finger gets PRESS_LOST instead of a click.
      end_press();
      // Left brings the next page, as on a phone, and the last page
      // wraps to the first. Pages outside the shown group are skipped.
      const int n = (int)ctx->screens.size();
      const int step = dx < 0 ? 1 : -1;
      int next = ctx->active;
      for (int k = 0; k < n; k++) {
        next = (next + step + n) % n;
        if (visible_in_group(ctx, next)) break;
      }
      switcher_show(ctx, next);
      strip_peek(ctx, true);  // shows where the swipe landed
      done = true;
    } else if (press_at_edge_ && -dy >= kEdgeSwipeMin && -dy > LV_ABS(dx)) {
      end_press();
      strip_peek(ctx, true);
      done = true;
    }
  }
  if (done) cancel_swipe();
}

ApplyResult LayoutManager::apply(const std::string& json, ApplySource src) {
  ApplyResult r{false, "", "", 0, 0, ""};
  if (!parent_) { r.err = "layout_manager not initialised"; return r; }

  if (json.size() > kMaxJsonBytes) {
    r.err = "layout too large";
    return r;
  }

  JsonDocument doc;
  DeserializationError de = deserializeJson(doc, json);
  if (de) { r.err = std::string("parse: ") + de.c_str(); return r; }

  if (!validate(doc.as<JsonObjectConst>(), &r.err)) return r;

  // Atomic swap: build the new tree as a child of `parent_` but hidden,
  // then unhide on success and delete the old. If the build fails, the
  // current layout stays up untouched.
  //
  // (Earlier this code used lv_obj_create(NULL) which creates a SCREEN,
  // not a regular object — re-parenting a screen under another object
  // is undefined in LVGL 9 and resulted in widgets that never rendered.)
  lv_obj_t* staging = lv_obj_create(parent_);
  lv_obj_set_size(staging, lv_pct(100), lv_pct(100));
  lv_obj_set_pos(staging, 0, 0);
  lv_obj_set_style_border_width(staging, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(staging, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(staging, 0, LV_PART_MAIN);
  lv_obj_set_scrollable(staging, false);
  lv_obj_set_hidden(staging, true);

  // Screen background: `theme.bg`, or transparent (falls through to
  // LVGL's default dark theme) when omitted. Set on the layout root
  // itself rather than a global screen style, so it's swapped
  // atomically with the rest of the layout and never bleeds into the
  // next one.
  JsonObjectConst theme = doc["theme"];
  uint32_t theme_bg = 0;
  if (!theme.isNull() &&
      parse_hex_color(theme["bg"] | (const char*)nullptr, &theme_bg)) {
    lv_obj_set_style_bg_color(staging, lv_color_hex(theme_bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(staging, LV_OPA_COVER, LV_PART_MAIN);
  } else {
    lv_obj_set_style_bg_opa(staging, LV_OPA_TRANSP, LV_PART_MAIN);
  }

  // Default fg/accent for bars, arcs and buttons — must run before
  // build_screens() below so newly built widgets pick up the layout's
  // theme immediately. These are process-wide, so a build that fails
  // has to put them back: the previous layout stays on screen, and its
  // still-live widgets would otherwise read the REJECTED layout's theme
  // on their next update.
  const ThemeColors prev_theme = current_theme();
  apply_theme(theme);

  std::set<std::string> live_paths;
  JsonArrayConst screens = doc["screens"];
  if (!build_screens(staging, doc.as<JsonObjectConst>(), screens, registry(),
                     &r.err, &live_paths, &r.widgets)) {
    restore_theme(prev_theme);
    lv_obj_delete(staging);
    return r;
  }

  // Apply status_overlay flag before the swap. Default true (matches
  // the original always-on behavior — existing layouts unchanged).
  // Toggling resizes content_root, so the staging tree needs to be
  // re-parented after the resize to inherit the new geometry.
  bool overlay_visible = true;
  if (doc["status_overlay"].is<bool>()) {
    overlay_visible = doc["status_overlay"];
  }
  overlay().set_visible(overlay_visible);

  // Re-configure the alert overlay from the layout's top-level
  // `notifications` block. Defaults: enabled + min_state alarm.
  // `levels` (optional) overrides, per state, what min_state implies:
  // show, sound (off / source / always) and repeat_s. A state it leaves
  // out keeps the min_state default, so an older layout behaves as before.
  bool notif_enabled = true;
  NotState notif_min = NotState::Alarm;
  JsonObjectConst notif = doc["notifications"];
  if (!notif.isNull()) {
    if (notif["enabled"].is<bool>()) notif_enabled = notif["enabled"];
    notif_min = parse_not_state(notif["min_state"] | "alarm");
  }
  LevelPolicies levels = policies_from_min_state(notif_min);
  JsonObjectConst lv = notif["levels"];
  static constexpr const char* kLevelNames[] = {"alert", "warn", "alarm", "emergency"};
  for (size_t i = 0; i < levels.size() && !lv.isNull(); ++i) {
    JsonObjectConst l = lv[kLevelNames[i]];
    if (l.isNull()) continue;
    if (l["show"].is<bool>()) levels[i].show = l["show"];
    const char* snd = l["sound"] | (const char*)nullptr;
    if (snd && !strcmp(snd, "off")) levels[i].sound = SoundPolicy::Off;
    if (snd && !strcmp(snd, "source")) levels[i].sound = SoundPolicy::Source;
    if (snd && !strcmp(snd, "always")) levels[i].sound = SoundPolicy::Always;
    if (l["repeat_s"].is<double>()) {
      // Read as a number, so a value past the int range clamps rather
      // than leaving the level at one chime.
      const double r = l["repeat_s"];
      levels[i].repeat_s = static_cast<uint16_t>(r < 0 ? 0 : (r > 3600 ? 3600 : r));
    }
  }
  alert_overlay().configure(notif_enabled, levels);

  // Apply the backlight idle timeout (0 = disabled) and the dim-while-
  // idle brightness. Default dim_pct=0 (fully off); the wake overlay
  // takes the first tap so nothing underneath fires by accident.
  uint32_t idle_timeout_sec = 0;
  uint8_t idle_dim_pct = 0;
  JsonObjectConst display = doc["display"];
  if (!display.isNull()) {
    if (display["idle_timeout_sec"].is<uint32_t>()) {
      idle_timeout_sec = display["idle_timeout_sec"];
    }
    if (display["idle_dim_pct"].is<uint8_t>()) {
      idle_dim_pct = display["idle_dim_pct"];
    }
  }
  idle_dimmer().configure(idle_timeout_sec, idle_dim_pct);

  // A swipe in progress belongs to the old layout: further movement must
  // not turn a page of the new one.
  cancel_swipe();

  lv_obj_t* old_root = current_root_;
  lv_obj_set_hidden(staging, false);
  current_root_ = staging;
  if (old_root) lv_obj_delete(old_root);
  resolve_groups();

  // Single-screen layouts have no switcher, so nothing else ever marks
  // their widgets visible. Multi-screen roots got switcher_show(0) during
  // the staging build — a stream widget's start then races the old
  // layout's teardown and retries on its watch timer, so no extra call is
  // needed there.
  if (screens.size() == 1) {
    stream_widgets_notify_visibility(current_root_, true);
  }

  // Persist only after a successful swap, and only for pushed layouts
  // (boot paths re-read the same persisted blob).
  if (src == ApplySource::PostLayout) {
    if (!store_write_atomic(json)) {
      r.warning = "layout rendered but not persisted";
    }
  }

  active_name_ = doc["name"] | "(unnamed)";
  active_source_ = src;

  // Compare against the previously-known path set so the post-swap
  // hook can decide whether to restart the WS (needed only if the
  // new layout introduced paths SK isn't currently subscribed to).
  // Capture the diff itself too — the hook uses it to fetch SK meta
  // for just the new paths instead of re-fetching everything.
  std::set<std::string> introduced;
  for (const auto& p : live_paths) {
    if (known_paths_.find(p) == known_paths_.end()) {
      introduced.insert(p);
    }
  }
  bool new_paths = !introduced.empty();
  known_paths_ = live_paths;
  if (post_swap_) post_swap_(new_paths, introduced, src, live_paths);

  r.ok = true;
  r.name = active_name_;
  r.screens = screens.size();
  ESP_LOGI(TAG,
           "applied layout '%s' (src=%d screens=%u widgets=%u paths=%u "
           "new=%d)",
           r.name.c_str(), (int)src, r.screens, r.widgets,
           (unsigned)live_paths.size(), new_paths ? 1 : 0);
  return r;
}

// The remote choice wins over a swipe still being tracked. When it changes
// the page, a press on the layout ends too: the widget gets PRESS_LOST, not
// a click, so a toggle on a page no longer shown cannot send a PUT. A tap on
// the page that stays keeps its click, and a press on an overlay above the
// layout (alert, wake) is left alone. A swipe that met its threshold has
// ended its press already.
void LayoutManager::change_page(int idx) {
  ScreenSwitcherCtx* ctx = switcher_of(current_root_);
  if (!ctx) return;
  cancel_swipe();
  if (idx != ctx->active && press_on_layout_ && indev_ != nullptr &&
      lv_indev_get_state(indev_) == LV_INDEV_STATE_PRESSED) {
    end_press();
  }
  switcher_show(ctx, idx);
}

bool LayoutManager::select_screen(const std::string& id) {
  ScreenSwitcherCtx* ctx = switcher_of(current_root_);
  if (!ctx) return false;
  for (size_t i = 0; i < ctx->ids.size(); i++) {
    if (ctx->ids[i] == id) {
      // A page of another group brings its group along.
      if (!visible_in_group(ctx, (int)i)) {
        for (size_t g = 0; g < ctx->members.size(); g++) {
          if (contains(ctx->members[g], (int)i)) {
            group_ = ctx->group = (int)g;
            show_group_tabs(ctx, groups_);
            break;
          }
        }
      }
      change_page((int)i);
      strip_peek(ctx, false);  // as a tab tap does
      return true;
    }
  }
  return false;
}

// Where an old group is in a new list: at the same row if that row has its
// name, else the first row with its name, -1 if none. Names may repeat, so
// a row that kept its place wins over one further up with the same name.
static int find_group(const std::vector<ScreenGroup>& groups, size_t old_row,
                      const std::string& name) {
  if (old_row < groups.size() && groups[old_row].name == name) return (int)old_row;
  for (size_t g = 0; g < groups.size(); g++) {
    if (groups[g].name == name) return (int)g;
  }
  return -1;
}

void LayoutManager::set_groups(std::vector<ScreenGroup> groups) {
  // A reload must not move the crew: the chosen group and each group's last
  // page are found again (find_group, page by id), so reordering rows or
  // changing an unrelated setting keeps both.
  ScreenSwitcherCtx* ctx = switcher_of(current_root_);
  std::vector<std::string> old_names;
  for (const auto& g : groups_) old_names.push_back(g.name);
  std::vector<std::pair<size_t, std::string>> last;  // old row, page id
  if (ctx) {
    for (size_t g = 0; g < ctx->last.size() && g < groups_.size(); g++) {
      if (ctx->last[g] >= 0) last.emplace_back(g, ctx->ids[ctx->last[g]]);
    }
  }
  const int old_group = group_;
  groups_ = std::move(groups);
  group_ = 0;
  if (old_group >= 0 && old_group < (int)old_names.size()) {
    const int g = find_group(groups_, old_group, old_names[old_group]);
    if (g >= 0) group_ = g;
  }
  resolve_groups();
  if (!ctx || ctx->members.empty()) return;
  for (const auto& [row, id] : last) {
    const int g = find_group(groups_, row, old_names[row]);
    if (g < 0) continue;
    for (int i : ctx->members[g]) {
      if (ctx->ids[i] == id) ctx->last[g] = i;
    }
  }
  if (ctx->group >= 0) ctx->last[ctx->group] = ctx->active;
}

// Matches the configured groups to the current layout's pages and puts
// the strip and the page in line with the chosen group.
void LayoutManager::resolve_groups() {
  ScreenSwitcherCtx* ctx = switcher_of(current_root_);
  if (!ctx) return;
  const int n = (int)ctx->ids.size();
  std::vector<bool> listed(n, false);
  std::vector<std::vector<int>> own(groups_.size());
  for (size_t g = 0; g < groups_.size(); g++) {
    for (int i = 0; i < n; i++) {
      const auto& p = groups_[g].pages;
      if (std::find(p.begin(), p.end(), ctx->ids[i]) != p.end()) {
        own[g].push_back(i);
        listed[i] = true;
      }
    }
  }
  ctx->members.assign(groups_.size(), {});
  bool any = false;
  for (size_t g = 0; g < groups_.size(); g++) {
    if (own[g].empty()) continue;  // the layout has none of its pages
    any = true;
    // Its own pages and the unlisted ones, in tab order.
    for (int i = 0; i < n; i++) {
      if (!listed[i] || contains(own[g], i)) ctx->members[g].push_back(i);
    }
  }
  if (!any) ctx->members.clear();
  ctx->last.assign(ctx->members.size(), -1);
  if (ctx->members.empty()) {
    ctx->group = -1;
  } else if (group_ < (int)ctx->members.size() &&
             !ctx->members[group_].empty()) {
    ctx->group = group_;
  } else {
    ctx->group = next_usable_group(ctx, -1);
  }
  show_group_tabs(ctx, groups_);
  if (!visible_in_group(ctx, ctx->active)) {
    change_page(ctx->members[ctx->group].front());
  } else if (ctx->group >= 0) {
    ctx->last[ctx->group] = ctx->active;
  }
}

bool LayoutManager::select_group(int g) {
  if (g < 0 || g >= (int)groups_.size()) return false;
  group_ = g;
  ScreenSwitcherCtx* ctx = switcher_of(current_root_);
  if (!ctx || g >= (int)ctx->members.size() || ctx->members[g].empty()) {
    return false;
  }
  if (ctx->group == g) return true;
  ctx->group = g;
  show_group_tabs(ctx, groups_);
  // Back to where the crew was in that group; the first time, a page the
  // two groups share stays, else the group's first page.
  int page = ctx->last[g];
  if (page < 0) {
    page = visible_in_group(ctx, ctx->active) ? ctx->active
                                              : ctx->members[g].front();
  }
  change_page(page);
  return true;
}

void LayoutManager::next_group() {
  ScreenSwitcherCtx* ctx = switcher_of(current_root_);
  if (!ctx || ctx->group < 0) return;
  const int g = next_usable_group(ctx, ctx->group);
  if (g < 0 || g == ctx->group) return;
  ESP_LOGI(TAG, "group %s by hand", groups_[g].name.c_str());
  select_group(g);
  // The strip stays up to pick a page of the new group.
  if (ctx->peeking) ctx->peek_start = lv_tick_get();
}

std::string LayoutManager::active_group_name() const {
  ScreenSwitcherCtx* ctx = switcher_of(current_root_);
  if (!ctx || ctx->group < 0 || ctx->group >= (int)groups_.size()) return "";
  return groups_[ctx->group].name;
}

std::string LayoutManager::active_screen() const {
  ScreenSwitcherCtx* ctx = switcher_of(current_root_);
  if (!ctx) return "";
  if (ctx->active < 0 || ctx->active >= (int)ctx->ids.size()) return "";
  return ctx->ids[ctx->active];
}

std::vector<std::string> LayoutManager::screen_ids() const {
  ScreenSwitcherCtx* ctx = switcher_of(current_root_);
  if (!ctx) return {};
  return ctx->ids;
}

LayoutManager& layout_manager() {
  static LayoutManager m;
  return m;
}

}  // namespace jlp

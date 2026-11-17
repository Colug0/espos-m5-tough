/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/* From espos-p4-cockpit main/jlp/layout/layout_manager.h, changed for the M5Stack Tough (main/jlp/README.md). */
#pragma once

#include "lvgl.h"
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace jlp {

// tab_strip values this player draws: what the layout parser accepts
// and what /hello advertises.
inline constexpr const char* kTabStripFixed = "fixed";
inline constexpr const char* kTabStripOverlayBottom = "overlay-bottom";
inline constexpr const char* kTabStripModes[] = {kTabStripFixed,
                                                 kTabStripOverlayBottom};

enum class ApplySource { Boot, BootStore, BootDefault, PostLayout, BootFetched };

// A set of pages the tab strip shows together (screen_groups settings).
// `pages` are screen ids; `states` are the values of the state path that
// select the group.
struct ScreenGroup {
  std::string name;
  std::vector<std::string> states;
  std::vector<std::string> pages;
};

struct ApplyResult {
  bool ok;
  std::string err;
  std::string name;       // root "name" field on success
  unsigned screens;
  unsigned widgets;
  std::string warning;    // non-fatal note, e.g. persistence failed
};

class LayoutManager {
 public:
  // `parent` is where the screens/tabview goes. Set once at boot.
  void init(lv_obj_t* parent);

  // Fires after every successful apply + swap. `new_paths` is the set
  // of bound paths the new layout introduced that weren't already in
  // the previous one (or all paths on first apply); the bool says
  // whether it is non-empty. espOS subscribes new paths by itself, but
  // SK streams only on change, so the hook seeds their values via REST.
  // `src` and `all_paths` let the hook re-seed values on a boot apply
  // (BootStore / BootFetched), where no paths are "new" but the quiet
  // ones — e.g. navigation.anchor.maxRadius — still need backfilling
  // because their delta was missed before the WS subscribed.
  using PostSwapHook =
      std::function<void(bool new_paths_introduced,
                         const std::set<std::string>& new_paths,
                         ApplySource src,
                         const std::set<std::string>& all_paths)>;
  void set_post_swap_hook(PostSwapHook h) { post_swap_ = std::move(h); }

  // Parses, validates, builds, and (on success) swaps in the new
  // layout. On failure, the current layout stays up.
  ApplyResult apply(const std::string& json, ApplySource src);

  const std::string& active_name() const { return active_name_; }
  ApplySource active_source() const { return active_source_; }

  // Remote equivalent of tapping a tab. Addressed by the layout's
  // `screens[].id` because the index is not stable across layout edits.
  // False if the id is unknown, or if the active layout is single-screen
  // (no tab strip exists, so there is nothing to select).
  //
  // UI TASK ONLY — all three touch LVGL. Callers on the HTTP task must
  // marshal with tough_hal::ui::post().
  bool select_screen(const std::string& id);

  // Id of the screen currently shown; empty for a single-screen layout.
  // Lets a caller see what the helm is on before changing it, and put it
  // back afterwards.
  std::string active_screen() const;

  // Screen ids of the active layout, in tab order. Empty when
  // single-screen.
  std::vector<std::string> screen_ids() const;

  // Page groups. The strip and page swipes then show the pages of one
  // group plus the pages no group lists; a tab at the strip's right end
  // switches to the next group. A group none of whose pages the layout
  // has is skipped. Kept across layout pushes. UI TASK ONLY.
  void set_groups(std::vector<ScreenGroup> groups);

  // Shows group `g` (index into set_groups) on the page last shown in it.
  // Remembered for the next layout if this one lacks the group. False if
  // the layout cannot show it. UI TASK ONLY.
  bool select_group(int g);

  // Name of the group shown; empty when the layout is ungrouped.
  std::string active_group_name() const;

  // The group tab's tap: the next group the layout can show.
  void next_group();

  // How long an overlay tab strip stays up after a swipe without a tap.
  void set_tab_peek_seconds(uint32_t seconds) { tab_peek_ms_ = seconds * 1000u; }

  // Paths bound by the currently-applied layout. Used by boot to seed
  // the values of a layout that was applied (from the store) before the
  // network — and thus before the post-swap hook — was ready.
  const std::set<std::string>& known_paths() const { return known_paths_; }

 private:
  // Page swipes on every multi-screen layout, and the swipe up that
  // shows an overlay tab strip.
  void on_press();
  void track_swipe();
  void cancel_swipe();
  void end_press();
  // A page change that did not come from the finger on the layout.
  void change_page(int idx);
  void resolve_groups();

  lv_obj_t* parent_ = nullptr;
  lv_indev_t* indev_ = nullptr;
  lv_point_t press_{};
  bool press_at_edge_ = false;
  bool press_on_layout_ = false;  // the current press began on the layout
  lv_obj_t* pressed_ = nullptr;   // what LVGL holds as pressed; may be gone
  uint32_t swipe_timer_ = 0;
  uint32_t tab_peek_ms_ = 5000;
  lv_obj_t* current_root_ = nullptr;
  std::string active_name_;
  ApplySource active_source_ = ApplySource::Boot;
  PostSwapHook post_swap_;
  std::vector<ScreenGroup> groups_;
  int group_ = 0;  // index into groups_ chosen last, by hand or by state
  // Paths the currently-applied layout binds. Used to detect whether
  // the next swap introduces brand-new paths, whose values the post-swap
  // hook seeds via REST.
  std::set<std::string> known_paths_;
};

LayoutManager& layout_manager();

}  // namespace jlp

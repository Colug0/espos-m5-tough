/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "layout/layout_manager.h"

namespace jlp {

// The screen_groups settings, and the Signal K state path that picks a
// group. A state that a group lists switches to it once it has held for
// hold_s; the first state after a restart or a settings change switches
// at once. A group chosen by hand, by the group tab or POST /screen, stays
// until the state selects another group. Unlisted values and null change
// nothing.
//
// UI TASK ONLY, the stream callback included: it copies the value and
// posts.
class ScreenGroups {
 public:
  // Reads the settings and subscribes the path. Once, after the boot
  // layout is applied.
  void init();
  // Re-reads the settings after a change (config change feed).
  void reload();
  // A value of subscription `gen`, posted by the stream callback;
  // `rx_us` is when the stream task received it.
  void deliver(uint32_t gen, const std::string& value, int64_t rx_us);
  // The state path's current value from a REST GET that started at
  // `requested_us`. Signal K streams only changes, so a state set hours
  // ago arrives this way. Dropped if a stream value is newer or the path
  // has changed since.
  void seed(const std::string& path, const std::string& value, int64_t requested_us);
  // The subscribed state path, empty without groups.
  const std::string& path() const { return path_; }

 private:
  void on_state(const std::string& value);
  void commit(int g);
  void tick();

  std::vector<ScreenGroup> groups_;
  bool auto_ = true;
  uint32_t hold_ms_ = 30000;
  std::string path_;
  int sub_ = 0;
  // Which subscription a value belongs to; an old path's late value is
  // dropped.
  uint32_t gen_ = 0;
  std::string state_;     // last value of the path
  int64_t stream_rx_us_ = 0;  // when the stream last delivered it, 0 none
  int state_group_ = -1;  // group the last held state selected
  int pending_ = -1;      // group a newer state selects, still holding
  uint32_t pending_since_ = 0;
};

ScreenGroups& screen_groups();

}  // namespace jlp

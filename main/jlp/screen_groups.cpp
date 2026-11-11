/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
#include "screen_groups.h"

#include <ArduinoJson.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_sk.h"
#include "net/zone_fetch.h"
#include "tough_hal/ui.h"

static const char* TAG = "jlp.groups";

namespace jlp {

namespace {

// A table cell as text. The web UI stores a cell that looks like a number
// as a number.
std::string cell_text(JsonVariantConst v) {
  if (v.is<const char*>()) return v.as<const char*>();
  if (v.isNull()) return "";
  std::string out;
  serializeJson(v, out);
  return out;
}

// A table cell, "a, b,c" or a JSON array, as a list without blanks.
std::vector<std::string> split_list(JsonVariantConst v) {
  std::vector<std::string> out;
  auto add = [&out](std::string s) {
    const size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return;
    s = s.substr(b, s.find_last_not_of(" \t") - b + 1);
    out.push_back(std::move(s));
  };
  if (v.is<JsonArrayConst>()) {
    for (JsonVariantConst e : v.as<JsonArrayConst>()) add(cell_text(e));
    return out;
  }
  const std::string text = cell_text(v);
  const char* s = text.c_str();
  std::string cur;
  for (const char* p = s; *p != '\0'; p++) {
    if (*p == ',') {
      add(std::move(cur));
      cur.clear();
    } else {
      cur += *p;
    }
  }
  add(std::move(cur));
  return out;
}

std::vector<ScreenGroup> read_groups() {
  std::vector<ScreenGroup> groups;
  // A table can be up to the NVS string limit; this buffer is above the
  // internal-RAM threshold, so it comes from PSRAM.
  std::vector<char> buf(4000, '\0');
  if (espos_config_get_str(ESPOS_CFG_NS_SCREEN_GROUPS, ESPOS_CFG_SCREEN_GROUPS_GROUPS,
                           buf.data(), buf.size(), nullptr) != ESP_OK) {
    return groups;
  }
  JsonDocument doc;
  if (deserializeJson(doc, buf.data()) != DeserializationError::Ok ||
      !doc.is<JsonArrayConst>()) {
    ESP_LOGW(TAG, "groups is not a JSON array of rows; no groups");
    return groups;
  }
  for (JsonVariantConst row : doc.as<JsonArrayConst>()) {
    // The web UI writes a row as cells in column order (name, states,
    // pages); a row written as an object by hand is read by name.
    const bool cells = row.is<JsonArrayConst>();
    ScreenGroup g;
    g.name = cell_text(cells ? row[0] : row["name"]);
    g.states = split_list(cells ? row[1] : row["states"]);
    g.pages = split_list(cells ? row[2] : row["pages"]);
    if (g.pages.empty()) continue;
    if (g.name.empty()) g.name = std::to_string(groups.size() + 1);
    groups.push_back(std::move(g));
  }
  return groups;
}

void on_update(const espos_sk_update_t* u, void* arg) {
  if (u->value_json == nullptr) return;
  // JSON text, decoded: "\"anchored\"" is anchored. null and numbers
  // keep their text and match no group. Copied: the string dies with the
  // callback.
  std::string v(u->value_json);
  JsonDocument doc;
  if (deserializeJson(doc, v) == DeserializationError::Ok && doc.is<const char*>()) {
    v = doc.as<const char*>();
  }
  const auto gen = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
  const int64_t rx_us = esp_timer_get_time();
  tough_hal::ui::post(
      [gen, v = std::move(v), rx_us] { screen_groups().deliver(gen, v, rx_us); });
}

}  // namespace

void ScreenGroups::init() {
  reload();
  tough_hal::ui::every(1000, [this] { tick(); });
}

void ScreenGroups::reload() {
  groups_ = read_groups();
  bool on = true;
  espos_config_get_bool(ESPOS_CFG_NS_SCREEN_GROUPS, ESPOS_CFG_SCREEN_GROUPS_AUTO, &on);
  int32_t hold_s = 30;
  espos_config_get_i32(ESPOS_CFG_NS_SCREEN_GROUPS, ESPOS_CFG_SCREEN_GROUPS_HOLD_S, &hold_s);
  char path[97] = "";
  espos_config_get_str(ESPOS_CFG_NS_SCREEN_GROUPS, ESPOS_CFG_SCREEN_GROUPS_PATH, path,
                       sizeof(path), nullptr);
  auto_ = on;
  hold_ms_ = static_cast<uint32_t>(hold_s) * 1000u;
  layout_manager().set_groups(groups_);

  // The path only matters while it may pick a group; unsubscribed
  // otherwise, since the subscription table is shared with the layout.
  // Turning the switch on reloads, which subscribes and fetches afresh.
  const std::string want = groups_.empty() || !auto_ ? "" : path;
  // A failed subscription (the table is shared and bounded) is tried
  // again on the next reload.
  if (want != path_ || (!want.empty() && sub_ <= 0)) {
    if (sub_ > 0) espos_sk_unsubscribe(sub_);
    sub_ = 0;
    path_ = want;
    gen_++;
    state_.clear();
    stream_rx_us_ = 0;
    seed_req_us_ = 0;
    path_since_us_ = esp_timer_get_time();
    if (!path_.empty()) {
      sub_ = espos_sk_subscribe(path_.c_str(), 1000, on_update,
                                reinterpret_cast<void*>(static_cast<uintptr_t>(gen_)));
      if (sub_ <= 0) ESP_LOGW(TAG, "subscribe %s failed (%d)", path_.c_str(), sub_);
      // The stream sends the next change only. Before Signal K is up the
      // first connection's fetch carries the path (app_main).
      espos_sk_server_t srv;
      if (espos_sk_get_server(&srv) == ESP_OK) zone_fetch_for_paths({path_});
    }
  }
  ESP_LOGI(TAG, "%u groups, auto %s, hold %d s, path %s", (unsigned)groups_.size(),
           auto_ ? "on" : "off", (int)hold_s, path_.empty() ? "-" : path_.c_str());

  // New settings decide afresh: the current state switches at once.
  state_group_ = -1;
  pending_ = -1;
  if (!state_.empty()) on_state(state_);
}

void ScreenGroups::deliver(uint32_t gen, const std::string& value, int64_t rx_us) {
  if (gen != gen_) return;
  stream_rx_us_ = rx_us;
  state_ = value;
  on_state(value);
}

void ScreenGroups::seed(const std::string& path, const std::string& value,
                        int64_t requested_us) {
  // A GET from before the path was (re)chosen answers an older choice,
  // and of two overlapping fetches the one whose GET started later wins.
  if (path != path_ || requested_us < path_since_us_ || stream_rx_us_ > requested_us ||
      seed_req_us_ > requested_us) {
    return;
  }
  seed_req_us_ = requested_us;
  ESP_LOGI(TAG, "%s is %s", path_.c_str(), value.c_str());
  state_ = value;
  on_state(value);
}

void ScreenGroups::on_state(const std::string& value) {
  int g = -1;
  for (size_t i = 0; i < groups_.size() && g < 0; i++) {
    for (const auto& s : groups_[i].states) {
      if (strcasecmp(s.c_str(), value.c_str()) == 0) {
        g = (int)i;
        break;
      }
    }
  }
  if (g < 0 || g == state_group_) {
    // Back to the held state, or one no group lists: nothing pending.
    pending_ = -1;
    return;
  }
  if (state_group_ < 0 || hold_ms_ == 0) {
    commit(g);
    return;
  }
  if (pending_ != g) {
    pending_ = g;
    pending_since_ = lv_tick_get();
  }
}

void ScreenGroups::tick() {
  if (pending_ >= 0 && lv_tick_elaps(pending_since_) >= hold_ms_) commit(pending_);
  // A subscription that failed at boot gets another go now and then.
  if (!path_.empty() && sub_ <= 0 && ++retry_ticks_ >= 30) {
    retry_ticks_ = 0;
    sub_ = espos_sk_subscribe(path_.c_str(), 1000, on_update,
                              reinterpret_cast<void*>(static_cast<uintptr_t>(gen_)));
    if (sub_ > 0) ESP_LOGI(TAG, "subscribed %s", path_.c_str());
  }
}

void ScreenGroups::commit(int g) {
  state_group_ = g;
  pending_ = -1;
  ESP_LOGI(TAG, "%s=%s selects %s%s", path_.c_str(), state_.c_str(), groups_[g].name.c_str(),
           auto_ ? "" : " (auto off)");
  if (auto_) layout_manager().select_group(g);
}

ScreenGroups& screen_groups() {
  static ScreenGroups s;
  return s;
}

}  // namespace jlp

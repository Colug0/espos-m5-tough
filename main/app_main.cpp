/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * espos-m5-tough: application entry. Adapted from espos-p4-cockpit's
 * main/app_main.cpp: the same JSON Layout Player wiring without the voice
 * satellite and the NMEA 2000 gateway, on the Tough's HAL.
 *
 * espOS provides WiFi, the config store and web UI, Signal K discovery,
 * token, stream and REST, mDNS, logs, core dump, signed OTA and the device
 * watchdog. This file brings up the board (AXP192, display, touch, speaker),
 * the player (jlp/) with its layout API on :8081, and the overlays.
 */
#include <atomic>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "espos.h"
#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_event.h"
#include "espos_mdns.h"
#include "espos_sk.h"
#include "espos_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jlp/alert_overlay.h"
#include "jlp/audio/chime.h"
#include "jlp/default_layout.h"
#include "jlp/idle_dimmer.h"
#include "jlp/layout/layout_manager.h"
#include "jlp/layout/store.h"
#include "jlp/net/http_api.h"
#include "jlp/net/layout_fetch.h"
#include "jlp/net/zone_fetch.h"
#include "jlp/notifications_registry.h"
#include "jlp/status_overlay.h"
#include "jlp/sun_state.h"
#include "jlp/wake_overlay.h"
#include "jlp/zone_registry.h"
#include "tough_hal/axp192.h"
#include "tough_hal/board.h"
#include "tough_hal/tough_audio.h"
#include "tough_hal/tough_display.h"
#include "tough_hal/ui.h"

static const char* TAG = "m5_tough";

namespace {

namespace ui = tough_hal::ui;

tough_hal::ToughDisplay s_display;
tough_hal::ToughTouch s_touch;
tough_hal::ToughAudio s_audio;
std::atomic<bool> s_panel_up{false};
bool s_boot_fetch_done = false;
bool s_sk_connected = false;

// Internal byte-addressable RAM is the pool that runs out on this chip
// (espos-memory-budget): count it, its largest block and its low-water mark
// separately from PSRAM.
void log_heap(const char* stage) {
  constexpr uint32_t kInternal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  ESP_LOGI(TAG, "heap %-10s internal %6u free, %6u largest, %6u min | PSRAM %7u free",
           stage, static_cast<unsigned>(heap_caps_get_free_size(kInternal)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(kInternal)),
           static_cast<unsigned>(heap_caps_get_minimum_free_size(kInternal)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

// All on the UI task.
void sk_stream_up() {
  s_sk_connected = true;
  jlp::overlay().set_sk("ok");
  jlp::overlay().hide_sk_lost();
  espos_sk_server_t srv;
  if (espos_sk_get_server(&srv) == ESP_OK) jlp::overlay().set_sk_server(srv.host, srv.port);
  // First connection after boot: seed zones and quiet values for the paths
  // the restored layout binds, then look for a layout in applicationData.
  if (!s_boot_fetch_done) {
    s_boot_fetch_done = true;
    const auto& boot_paths = jlp::layout_manager().known_paths();
    if (!boot_paths.empty()) {
      jlp::zone_fetch_for_paths(std::vector<std::string>(boot_paths.begin(), boot_paths.end()));
    }
    jlp::layout_fetch_async_apply();
  }
}

void sk_stream_down() {
  s_sk_connected = false;
  jlp::overlay().set_sk("down");
  jlp::overlay().show_sk_lost();
}

void on_espos_event(void*, esp_event_base_t, int32_t id, void*) {
  if (!s_panel_up) return;  // headless: no overlay to update
  switch (id) {
    case ESPOS_EVENT_SK_STREAM_CONNECTED:
      ui::post(sk_stream_up);
      break;
    case ESPOS_EVENT_SK_STREAM_DISCONNECTED:
      ui::post(sk_stream_down);
      break;
    case ESPOS_EVENT_SK_TOKEN_APPROVED:
      ui::post([] {
        if (!s_sk_connected) jlp::overlay().set_sk("approved");
      });
      break;
    case ESPOS_EVENT_PORTAL_UP: {
      espos_wifi_status_t st = {};
      espos_wifi_get_status(&st);
      std::string ssid = st.portal_ssid;
      ui::post([ssid] { jlp::overlay().set_wifi(("setup " + ssid).c_str()); });
      break;
    }
    case ESPOS_EVENT_NETWORK_UP:
      ui::post([] {
        if (!s_sk_connected) jlp::overlay().set_sk("connecting");
      });
      break;
    case ESPOS_EVENT_NETWORK_DOWN:
      ui::post([] {
        if (!s_sk_connected) jlp::overlay().set_sk("no network");
      });
      break;
    default:
      break;
  }
}

// The ESP32 restarts without deauthenticating, so the access point still holds
// the previous association when the new one arrives. A MikroTik hAP ax S
// (RouterOS 7.24.4) then logs "reconnecting" instead of "connected", keeps
// the old station state, and for minutes delivers nothing to the device while
// its own frames and broadcasts get through (measured 2026-11-01: no ping from
// the router, no Signal K). Removing the station on the router fixed it at
// once. So the first association of every boot is dropped on purpose: the
// deauthentication makes the access point forget the old station, and espOS
// connects again straight away, now as a new one. Costs about two seconds.
// Belongs in espOS's WiFi handling once it is fixed there.
void on_wifi_event(void*, esp_event_base_t, int32_t id, void*) {
  static std::atomic<bool> done{false};
  if (id != WIFI_EVENT_STA_CONNECTED || done.exchange(true)) return;
  ESP_LOGI(TAG, "dropping the first association so the access point starts a fresh one");
  esp_wifi_disconnect();
}

// The 320 px strip has room for the signal strength, not the SSID.
void poll_status_line() {
  espos_wifi_status_t w = {};
  if (espos_wifi_get_status(&w) == ESP_OK && w.sm.state == ESPOS_WIFI_ST_CONNECTED) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%ddBm", static_cast<int>(w.rssi));
    jlp::overlay().set_wifi(buf);
  } else if (w.portal_ssid[0] == '\0') {
    jlp::overlay().set_wifi("down");
  }
}

// Runs on the UI task once the panel is up.
void player_up() {
  char host[33] = "";
  espos_config_get_str(ESPOS_CFG_NS_WIFI, ESPOS_CFG_WIFI_HOSTNAME, host, sizeof(host), nullptr);
  jlp::overlay().init();
  jlp::overlay().set_hostname(host[0] ? host : "m5-tough");
  jlp::overlay().set_sk("no network");  // events take it from here
  jlp::layout_manager().init(jlp::overlay().content_root());
  jlp::store_init();
  jlp::chime().init(&s_audio);

  // Boot priority (JLP-PROTOCOL.md): the stored layout, else the compiled-in
  // default. A layout from applicationData follows once Signal K is up.
  std::string stored;
  jlp::ApplyResult r{};
  if (jlp::store_read(&stored)) {
    r = jlp::layout_manager().apply(stored, jlp::ApplySource::BootStore);
    if (!r.ok) {
      ESP_LOGW(TAG, "stored layout rejected (%s); clearing + falling back", r.err.c_str());
      jlp::store_clear();
    }
  }
  if (!r.ok) {
    r = jlp::layout_manager().apply(jlp::kDefaultLayoutJson, jlp::ApplySource::BootDefault);
    if (!r.ok) ESP_LOGE(TAG, "default layout rejected: %s", r.err.c_str());
  }

  // Zones and quiet values for paths a new layout introduces; on a boot
  // source, for every path.
  jlp::layout_manager().set_post_swap_hook(
      [](bool /*new_paths_introduced*/, const std::set<std::string>& new_paths, jlp::ApplySource src,
         const std::set<std::string>& all_paths) {
        const bool boot = src == jlp::ApplySource::BootStore || src == jlp::ApplySource::BootFetched;
        const std::set<std::string>& to_fetch = boot ? all_paths : new_paths;
        if (to_fetch.empty()) return;
        espos_sk_server_t srv;
        if (espos_sk_get_server(&srv) == ESP_OK) {
          jlp::zone_fetch_for_paths(std::vector<std::string>(to_fetch.begin(), to_fetch.end()));
        }
      });

  jlp::zones().hook_sk_ws();
  jlp::notifications().hook_sk_ws();
  jlp::sun_state().hook_sk_ws();
  jlp::alert_overlay().init();
  jlp::wake_overlay().init();  // after alert_overlay: z-order
  jlp::idle_dimmer().init();
  (void)jlp::notifications().on_change([]() {
    if (jlp::notifications().last_change_was_escalation()) jlp::idle_dimmer().wake();
  });
  ui::every(1000, poll_status_line);
}

// espos_start()'s before_network hook: the display is up before the radio,
// so it can show the setup portal's SSID.
esp_err_t board_up(void*) {
  log_heap("boot");
  if (tough_hal::axp192::init() != ESP_OK) {
    // Without the PMIC the LCD has no power; carry on so the device still
    // reaches the network and its logs can be read over WiFi.
    ESP_LOGE(TAG, "AXP192 init failed, running headless");
    return ESP_OK;
  }
  if (tough_hal::axp192::reset_lcd_and_touch() != ESP_OK) {
    ESP_LOGE(TAG, "LCD/touch reset failed, running headless");
    return ESP_OK;
  }
  ui::start(&s_display, &s_touch);
  log_heap("ui");
  s_audio.init();
  log_heap("audio");

  int32_t pct = 60;
  espos_config_get_i32(ESPOS_CFG_NS_COCKPIT, ESPOS_CFG_COCKPIT_BRIGHTNESS, &pct);
  const auto brightness = static_cast<uint8_t>(pct);
  // A brightness change from the web UI or the designer becomes the level the
  // idle dimmer restores on wake; the brightness slider applies it itself.
  espos_config_subscribe(
      [](const char* ns, const char* key, void*) {
        if (strcmp(ns, ESPOS_CFG_NS_COCKPIT) != 0) return;
        if (strcmp(key, ESPOS_CFG_COCKPIT_BRIGHTNESS) != 0) return;
        int32_t v = 60;
        espos_config_get_i32(ESPOS_CFG_NS_COCKPIT, ESPOS_CFG_COCKPIT_BRIGHTNESS, &v);
        const auto b = static_cast<uint8_t>(v);
        ui::post([b] {
          s_display.set_brightness(b);
          jlp::idle_dimmer().set_on_brightness(b);
        });
      },
      nullptr);

  ui::post(player_up);
  // Light the backlight once the first frame is drawn, not on a blank panel.
  ui::after(150, [brightness] {
    s_display.set_brightness(brightness);
    jlp::idle_dimmer().set_on_brightness(brightness);
  });
  s_panel_up = true;
  return ESP_OK;
}

}  // namespace

extern "C" void app_main(void) {
  // Subscribed before anything can post, so the first NETWORK_UP and the
  // first stream connect are not missed.
  for (int32_t id : {ESPOS_EVENT_PORTAL_UP, ESPOS_EVENT_NETWORK_UP, ESPOS_EVENT_NETWORK_DOWN,
                     ESPOS_EVENT_SK_TOKEN_APPROVED, ESPOS_EVENT_SK_STREAM_CONNECTED,
                     ESPOS_EVENT_SK_STREAM_DISCONNECTED}) {
    ESP_ERROR_CHECK(espos_event_subscribe(id, on_espos_event, nullptr));
  }
  // The default loop exists now (espos_event_subscribe creates it if needed).
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, on_wifi_event, nullptr));

  espos_start_opts_t opts = ESPOS_START_OPTS_DEFAULT;
  opts.app_name = "m5-tough";
  opts.board = tough_hal::kBoardName;
  opts.before_network = board_up;
  ESP_ERROR_CHECK(espos_start(&opts));
  log_heap("espos");

  if (s_panel_up) {
    int32_t api_port = 8081;
    espos_config_get_i32(ESPOS_CFG_NS_COCKPIT, ESPOS_CFG_COCKPIT_API_PORT, &api_port);
    jlp::http_api_start(static_cast<uint16_t>(api_port));

    // The same capability summary /hello gives, for mDNS browsers (the
    // designer's device list). Keep the widget list in step with
    // kWidgetCatalogJson in jlp/net/http_api.cpp.
    static char fw_txt[64];
    snprintf(fw_txt, sizeof(fw_txt), "firmware=m5-tough-jlp-%s", esp_app_get_description()->version);
    static const char* const kPlayerTxt[] = {
        "schema=1",
        "widgets=label,value,toggle,arc,bar,bargroup,button,notifications,anchor,anchor_track,slider",
        fw_txt,
        "api=/layout,/hello,/healthz,/screenshot",
    };
    esp_err_t merr = espos_mdns_add_service("_signalk-player", "_tcp", static_cast<uint16_t>(api_port),
                                            kPlayerTxt, sizeof(kPlayerTxt) / sizeof(kPlayerTxt[0]));
    if (merr != ESP_OK) ESP_LOGW(TAG, "_signalk-player._tcp not advertised: %s", esp_err_to_name(merr));
    log_heap("jlp");
  }

  // The low-water mark is what matters for headroom; log it once a minute
  // with the UI task's stack high-water mark.
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(60000));
    log_heap("running");
    ESP_LOGI(TAG, "ui stack high-water %u bytes", static_cast<unsigned>(ui::stack_high_water()));
  }
}

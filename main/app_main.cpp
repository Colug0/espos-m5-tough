/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Phase 1 bring-up: power, display, touch and speaker on the Tough, espOS
 * underneath, and a test screen that shows the network state, touch
 * coordinates and the memory budget. The JSON Layout Player replaces the
 * test screen in phase 2.
 */
#include <cmath>
#include <string>
#include <vector>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "espos.h"
#include "espos_event.h"
#include "espos_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "tough_hal/axp192.h"
#include "tough_hal/board.h"
#include "tough_hal/tough_audio.h"
#include "tough_hal/tough_display.h"
#include "tough_hal/ui.h"

namespace {

const char* TAG = "m5_tough";

tough_hal::ToughDisplay s_display;
tough_hal::ToughTouch s_touch;
tough_hal::ToughAudio s_audio;
std::vector<int16_t> s_beep;

lv_obj_t* s_status = nullptr;
lv_obj_t* s_touch_label = nullptr;
lv_obj_t* s_heap = nullptr;

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

void make_beep() {
  // 880 Hz for 250 ms with 10 ms fades, so the amplifier does not click.
  const uint32_t rate = s_audio.sample_rate();
  const size_t n = rate / 4;
  const size_t fade = rate / 100;
  s_beep.resize(n);
  for (size_t i = 0; i < n; i++) {
    float env = 1.0f;
    if (i < fade) env = static_cast<float>(i) / fade;
    if (i > n - fade) env = static_cast<float>(n - i) / fade;
    s_beep[i] = static_cast<int16_t>(12000.0f * env *
                                     std::sin(2.0f * static_cast<float>(M_PI) * 880.0f * i / rate));
  }
}

void set_status(std::string text) {
  tough_hal::ui::post([text = std::move(text)] {
    if (s_status) lv_label_set_text(s_status, text.c_str());
  });
}

void on_espos_event(void*, esp_event_base_t, int32_t id, void* data) {
  switch (id) {
    case ESPOS_EVENT_PORTAL_UP: {
      espos_wifi_status_t st = {};
      espos_wifi_get_status(&st);
      set_status(std::string("Setup: join WiFi ") + st.portal_ssid + "\nthen open http://" +
                 st.portal_ip);
      break;
    }
    case ESPOS_EVENT_NETWORK_UP: {
      const auto* net = static_cast<const espos_event_network_t*>(data);
      set_status(std::string("WiFi: ") + net->ip + "\nSignal K: searching");
      break;
    }
    case ESPOS_EVENT_NETWORK_DOWN:
      set_status("WiFi: disconnected");
      break;
    case ESPOS_EVENT_SK_SERVER_SELECTED: {
      const auto* sk = static_cast<const espos_event_sk_server_t*>(data);
      set_status(std::string("Signal K: ") + sk->host + ":" + std::to_string(sk->port) +
                 "\nwaiting for access approval");
      break;
    }
    case ESPOS_EVENT_SK_STREAM_CONNECTED:
      set_status("Signal K: connected");
      break;
    case ESPOS_EVENT_SK_STREAM_DISCONNECTED:
      set_status("Signal K: disconnected");
      break;
    default:
      break;
  }
}

void update_heap_label() {
  constexpr uint32_t kInternal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  lv_label_set_text_fmt(s_heap, "internal %u KB (block %u, min %u) | PSRAM %u KB | ui stack %u",
                        static_cast<unsigned>(heap_caps_get_free_size(kInternal) / 1024),
                        static_cast<unsigned>(heap_caps_get_largest_free_block(kInternal) / 1024),
                        static_cast<unsigned>(heap_caps_get_minimum_free_size(kInternal) / 1024),
                        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                        static_cast<unsigned>(tough_hal::ui::stack_high_water()));
}

// Runs on the UI task.
void build_test_screen() {
  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d1117), 0);
  lv_obj_set_style_pad_all(scr, 8, 0);

  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text_fmt(title, "espos-m5-tough %s", esp_app_get_description()->version);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  s_status = lv_label_create(scr);
  lv_label_set_text(s_status, "Starting network...");
  lv_obj_set_width(s_status, 304);
  lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 0, 30);

  lv_obj_t* slider = lv_slider_create(scr);
  lv_slider_set_range(slider, 5, 100);
  lv_slider_set_value(slider, 60, LV_ANIM_OFF);
  lv_obj_set_width(slider, 180);
  lv_obj_align(slider, LV_ALIGN_LEFT_MID, 8, 22);
  lv_obj_add_event_cb(
      slider,
      [](lv_event_t* e) {
        auto* s = static_cast<lv_obj_t*>(lv_event_get_target(e));
        s_display.set_brightness(static_cast<uint8_t>(lv_slider_get_value(s)));
      },
      LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t* btn = lv_button_create(scr);
  lv_obj_set_size(btn, 96, 44);
  lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 22);
  lv_obj_t* btn_label = lv_label_create(btn);
  lv_label_set_text(btn_label, "Beep");
  lv_obj_center(btn_label);
  lv_obj_add_event_cb(
      btn, [](lv_event_t*) { s_audio.play_pcm(s_beep.data(), s_beep.size()); },
      LV_EVENT_CLICKED, nullptr);

  s_touch_label = lv_label_create(scr);
  lv_label_set_text(s_touch_label, "Touch: -");
  lv_obj_align(s_touch_label, LV_ALIGN_BOTTOM_LEFT, 0, -36);

  s_heap = lv_label_create(scr);
  lv_obj_set_width(s_heap, 304);
  lv_obj_set_style_text_font(s_heap, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_heap, lv_color_hex(0x8b949e), 0);
  lv_obj_align(s_heap, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  update_heap_label();

  lv_obj_set_clickable(scr, true);
  lv_obj_add_event_cb(
      scr,
      [](lv_event_t*) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        lv_label_set_text_fmt(s_touch_label, "Touch: %d, %d", static_cast<int>(p.x),
                              static_cast<int>(p.y));
      },
      LV_EVENT_PRESSING, nullptr);

  tough_hal::ui::every(2000, update_heap_label);
}

// espos_start()'s before_network hook: the display is up before the radio,
// so it can show the setup portal's SSID.
esp_err_t board_up(void*) {
  log_heap("boot");
  esp_err_t err = tough_hal::axp192::init();
  if (err != ESP_OK) {
    // Without the PMIC the LCD has no power; carry on so the device still
    // reaches the network and its logs can be read over WiFi.
    ESP_LOGE(TAG, "AXP192 init failed, running headless");
    return ESP_OK;
  }
  tough_hal::axp192::reset_lcd_and_touch();
  tough_hal::ui::start(&s_display, &s_touch);
  log_heap("ui");
  s_audio.init();
  make_beep();
  log_heap("audio");

  for (int32_t id : {ESPOS_EVENT_PORTAL_UP, ESPOS_EVENT_NETWORK_UP, ESPOS_EVENT_NETWORK_DOWN,
                     ESPOS_EVENT_SK_SERVER_SELECTED, ESPOS_EVENT_SK_STREAM_CONNECTED,
                     ESPOS_EVENT_SK_STREAM_DISCONNECTED}) {
    espos_event_subscribe(id, on_espos_event, nullptr);
  }

  tough_hal::ui::post(build_test_screen);
  // Light the backlight once the first frame is drawn, not on a blank panel.
  tough_hal::ui::after(100, [] { s_display.set_brightness(60); });
  return ESP_OK;
}

}  // namespace

extern "C" void app_main(void) {
  espos_start_opts_t opts = ESPOS_START_OPTS_DEFAULT;
  opts.app_name = "m5-tough";
  opts.board = tough_hal::kBoardName;
  opts.before_network = board_up;
  ESP_ERROR_CHECK(espos_start(&opts));
  log_heap("espos");

  // The low-water mark is what matters for headroom; log it once a minute.
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(60000));
    log_heap("running");
  }
}

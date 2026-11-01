/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * From espos-p4-cockpit (components/cockpit_hal/src/ui.cpp). Tough changes:
 * draw buffers come from the display driver (internal DMA RAM, as the ESP32's
 * SPI DMA cannot read PSRAM), pixels are byte-swapped for the SPI panel, no
 * 180 degree rotation, the tick comes from esp_timer_get_time(), and a timer
 * callback is held by a shared_ptr while it runs so it may cancel its own
 * timer.
 */
#include "tough_hal/ui.h"

#include <deque>
#include <map>
#include <memory>
#include <mutex>

#include "esp_log.h"
#include "esp_timer.h"
#include "espos_health.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char* TAG = "ui";

namespace tough_hal {
namespace ui {

namespace {

DisplayDriver* s_display = nullptr;
TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_lvgl_lock = nullptr;   // recursive: LVGL calls from other tasks
SemaphoreHandle_t s_started = nullptr;
std::mutex s_queue_mutex;
std::deque<std::function<void()>> s_queue;
uint32_t s_next_handle = 1;
std::map<uint32_t, lv_timer_t*> s_timers;

void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  if (s_display) {
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    // LVGL renders little-endian RGB565; the panel takes it big-endian.
    lv_draw_sw_rgb565_swap(px_map, static_cast<uint32_t>(w * h));
    s_display->flush(area->x1, area->y1, w, h, px_map);
    // The SPI transfer is asynchronous: hold the buffer until DMA is done.
    s_display->wait_flush_done();
  }
  lv_display_flush_ready(disp);
}

void indev_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
  auto* touch = static_cast<TouchDriver*>(lv_indev_get_user_data(indev));
  auto pt = touch->read();
  data->point.x = pt.x;
  data->point.y = pt.y;
  data->state = pt.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

uint32_t tick_cb() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void drain_queue() {
  for (;;) {
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> g(s_queue_mutex);
      if (s_queue.empty()) return;
      fn = std::move(s_queue.front());
      s_queue.pop_front();
    }
    fn();
  }
}

void ui_task(void*) {
  // espOS's watchdog policy raises taskStalled (fatal) when this loop stops
  // kicking for 15 s, and the IDF task watchdog panics at 30 s as the hard
  // stop. A layout build takes hundreds of ms; a stalled LVGL never returns.
  espos_health_watch_task("ui", 15000);
  xSemaphoreGive(s_started);
  for (;;) {
    xSemaphoreTakeRecursive(s_lvgl_lock, portMAX_DELAY);
    drain_queue();
    uint32_t wait = lv_timer_handler();
    xSemaphoreGiveRecursive(s_lvgl_lock);
    espos_health_kick();
    if (wait > 20) wait = 20;   // stay responsive to post()
    if (wait == 0) wait = 1;
    vTaskDelay(pdMS_TO_TICKS(wait));
  }
}

}  // namespace

void start(DisplayDriver* display, TouchDriver* touch) {
  s_display = display;
  s_lvgl_lock = xSemaphoreCreateRecursiveMutex();
  s_started = xSemaphoreCreateBinary();
  display->init();
  touch->init();
  lv_init();
  lv_tick_set_cb(tick_cb);
  ESP_LOGI(TAG, "LVGL %d.%d.%d", lv_version_major(), lv_version_minor(), lv_version_patch());

  lv_display_t* d = lv_display_create(display->width(), display->height());
  lv_display_set_flush_cb(d, flush_cb);
  lv_display_set_buffers(d, display->get_draw_buffer(0), display->get_draw_buffer(1),
                         display->get_draw_buffer_size(), LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t* in = lv_indev_create();
  lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(in, indev_read_cb);
  lv_indev_set_user_data(in, touch);

  // Big stack: widget_factory builds whole layouts on this thread; PSRAM
  // stacks are not allowed on the classic ESP32, so internal RAM.
  xTaskCreatePinnedToCore(ui_task, "ui", 16384, nullptr, 5, &s_task, 1);
  xSemaphoreTake(s_started, portMAX_DELAY);
  ESP_LOGI(TAG, "ui task running (%dx%d)", display->width(), display->height());
}

void post(std::function<void()> fn) {
  if (on_ui_thread()) {
    fn();
    return;
  }
  std::lock_guard<std::mutex> g(s_queue_mutex);
  s_queue.push_back(std::move(fn));
}

namespace {
struct TimerCtx {
  // Shared so that timer_cb can hold the callable while it runs: a callback
  // that cancels its own timer deletes ctx, but not the function executing.
  // One callable for every firing, so a mutable lambda keeps its state.
  std::shared_ptr<std::function<void()>> fn;
  uint32_t handle;
  bool once;
};
void timer_cb(lv_timer_t* t) {
  auto* ctx = static_cast<TimerCtx*>(lv_timer_get_user_data(t));
  const uint32_t handle = ctx->handle;
  const bool once = ctx->once;
  const auto fn = ctx->fn;
  (*fn)();
  // fn may have called cancel(handle) — then ctx is gone; check the map.
  auto it = s_timers.find(handle);
  if (it == s_timers.end()) return;
  if (once) {
    s_timers.erase(it);
    lv_timer_delete(t);
    delete ctx;
  }
}
uint32_t make_timer(uint32_t ms, std::function<void()> fn, bool once) {
  auto* ctx = new TimerCtx{std::make_shared<std::function<void()>>(std::move(fn)), 0, once};
  uint32_t h = 0;
  auto create = [&] {
    // 0 is the callers' "no timer" sentinel, and after a counter wrap a
    // recycled handle could still be live in the map — take the next free
    // nonzero one so cancel() can never target the wrong timer.
    do {
      if (s_next_handle == 0) s_next_handle = 1;
      ctx->handle = s_next_handle++;
    } while (s_timers.count(ctx->handle) != 0);
    lv_timer_t* t = lv_timer_create(timer_cb, ms ? ms : 1, ctx);
    if (once) lv_timer_set_repeat_count(t, 1);
    s_timers[ctx->handle] = t;
    h = ctx->handle;
  };
  if (on_ui_thread()) {
    create();
  } else {
    // creating LVGL timers must happen on the UI thread; hand it over and
    // reserve the handle synchronously so callers can cancel later
    ctx->handle = 0;
    xSemaphoreTakeRecursive(s_lvgl_lock, portMAX_DELAY);
    create();
    xSemaphoreGiveRecursive(s_lvgl_lock);
  }
  return h;
}
}  // namespace

void after(uint32_t ms, std::function<void()> fn) { make_timer(ms, std::move(fn), true); }
uint32_t every(uint32_t ms, std::function<void()> fn) { return make_timer(ms, std::move(fn), false); }

void cancel(uint32_t handle) {
  xSemaphoreTakeRecursive(s_lvgl_lock, portMAX_DELAY);
  auto it = s_timers.find(handle);
  if (it != s_timers.end()) {
    auto* ctx = static_cast<TimerCtx*>(lv_timer_get_user_data(it->second));
    lv_timer_delete(it->second);
    delete ctx;
    s_timers.erase(it);
  }
  xSemaphoreGiveRecursive(s_lvgl_lock);
}

bool on_ui_thread() { return xTaskGetCurrentTaskHandle() == s_task; }
void lock() { xSemaphoreTakeRecursive(s_lvgl_lock, portMAX_DELAY); }
void unlock() { xSemaphoreGiveRecursive(s_lvgl_lock); }
DisplayDriver* display() { return s_display; }

uint32_t stack_high_water() {
  return s_task ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s_task)) : 0;
}

}  // namespace ui
}  // namespace tough_hal

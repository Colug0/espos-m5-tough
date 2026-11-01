/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * From espos-p4-cockpit (components/cockpit_hal), namespace renamed.
 *
 * The UI thread: one FreeRTOS task owns LVGL (lv_timer_handler + all
 * lv_* calls). Everything that arrives from other tasks — SignalK stream
 * callbacks, HTTP handlers, timers — is marshalled here with ui::post().
 * ui::every()/ui::after() are LVGL timers, i.e. they also run here.
 */
#pragma once

#include <cstdint>
#include <functional>

#include "tough_hal/display_driver.h"

namespace tough_hal {
namespace ui {

/** Bring up display + touch + LVGL and start the UI task. Blocks until the
 * first lv_timer_handler() ran, so callers may create objects (via post)
 * right after. */
void start(DisplayDriver* display, TouchDriver* touch);

/** Run fn on the UI thread as soon as possible (queue, thread-safe, ISR-unsafe). */
void post(std::function<void()> fn);
/** Run fn once after ms (UI thread). */
void after(uint32_t ms, std::function<void()> fn);
/** Run fn every ms (UI thread). Returns a handle for cancel(). */
uint32_t every(uint32_t ms, std::function<void()> fn);
void cancel(uint32_t handle);

/** True when called from the UI thread. */
bool on_ui_thread();
/** Lock LVGL from another task for a short critical section (screenshot). */
void lock();
void unlock();

DisplayDriver* display();

/** Smallest free stack the UI task has had, in bytes (0 before start()). */
uint32_t stack_high_water();

}  // namespace ui
}  // namespace tough_hal

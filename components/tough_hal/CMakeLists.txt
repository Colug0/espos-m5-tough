# SPDX-FileCopyrightText: 2026 Thomas Christoffel
# SPDX-License-Identifier: Apache-2.0
idf_component_register(
    SRCS src/i2c_bus.cpp src/axp192.cpp src/tough_display.cpp src/tough_touch.cpp
         src/tough_audio.cpp src/ui.cpp
    INCLUDE_DIRS include
    REQUIRES espos_audio lvgl__lvgl esp_lcd esp_driver_i2c esp_driver_i2s esp_driver_gpio
    PRIV_REQUIRES esp_driver_spi esp_timer espos_health)

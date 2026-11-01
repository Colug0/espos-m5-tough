# SPDX-FileCopyrightText: 2026 Thomas Christoffel
# SPDX-License-Identifier: Apache-2.0
#
# espos-m5-tough: a pure ESP-IDF 6 project on espOS (espos/ submodule). WiFi,
# config, the web UI, Signal K, mDNS, the health watchdog and signed OTA come
# from espOS; this project adds the M5Stack Tough HAL, LVGL and the display.
#
# Build (WSL2, the path must not contain spaces):
#   . ~/esp/esp-idf-v6.0.3/export.sh
#   scripts/build.sh -B build -DIDF_TARGET=esp32 build
cmake_minimum_required(VERSION 3.22)

# The Tough carries 16 MB. A device keeps the table it was first flashed with
# (OTA replaces app slots, never the table), so a change here reaches only
# boards flashed over USB afterwards.
include("${CMAKE_CURRENT_LIST_DIR}/espos/cmake/espos_project.cmake")
espos_project_prologue(NAME "m5-tough"
                       PARTITIONS "${ESPOS_PARTITIONS_DIR}/16mb.csv")

# LVGL is configured by the HAL's lv_conf.h, not by Kconfig.
idf_build_set_property(COMPILE_DEFINITIONS "-DLV_CONF_PATH=\"${CMAKE_CURRENT_LIST_DIR}/components/tough_hal/lv_conf.h\"" APPEND)

# The registry "app" and the OTA project check. Never rename it: every device
# in the field would refuse the renamed firmware's updates.
project(m5_tough)

# The committed espOS web UI -> LittleFS "storage" image.
espos_project_ui_partition(NAME "m5-tough")

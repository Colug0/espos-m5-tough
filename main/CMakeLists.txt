# SPDX-FileCopyrightText: 2026 Thomas Christoffel
# SPDX-License-Identifier: Apache-2.0
#
# espos_ota and espos_sk are listed although app_main.cpp barely calls them:
# espOS consumers build with IDF's MINIMAL_BUILD, so a component nothing
# requires is not compiled, and espos_start() starts only what is in the
# build. Without espos_ota the display could never be updated in the field.
idf_component_register(
    SRCS app_main.cpp
    INCLUDE_DIRS "."
    PRIV_REQUIRES tough_hal espos_core espos_event espos_health espos_wifi espos_sk espos_ota
                  esp_app_format esp_timer lvgl__lvgl)

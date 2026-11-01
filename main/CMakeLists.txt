# SPDX-FileCopyrightText: 2026 Thomas Christoffel
# SPDX-License-Identifier: Apache-2.0
#
# espos_ota and espos_sk are listed although app_main.cpp barely calls them:
# espOS consumers build with IDF's MINIMAL_BUILD, so a component nothing
# requires is not compiled, and espos_start() starts only what is in the
# build. Without espos_ota the display could never be updated in the field.
file(GLOB_RECURSE jlp_sources ${CMAKE_CURRENT_LIST_DIR}/jlp/*.cpp)
idf_component_register(
    SRCS app_main.cpp jpeg_psram.c ${jlp_sources}
    INCLUDE_DIRS "."
    PRIV_REQUIRES tough_hal espos_core espos_event espos_health espos_net espos_wifi espos_sk
                  espos_ota espos_config espos_audio esp_app_format esp_event esp_timer esp_wifi
                  esp_http_server nvs_flash
                  lvgl__lvgl bblanchon__arduinojson espressif__esp_new_jpeg)

# The "cockpit" settings namespace (brightness, layout API port). After
# idf_component_register.
espos_config_add_descriptor(config/cockpit.json)

# The JPEG encoder's working buffers in PSRAM instead of internal RAM
# (jpeg_psram.c says why). --undefined pulls the replacements in before the
# linker reaches esp_new_jpeg, whose objects are the only callers.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=jpeg_calloc_inner" "-Wl,--wrap=jpeg_calloc_align_inner"
    "-Wl,--undefined=__wrap_jpeg_calloc_inner" "-Wl,--undefined=__wrap_jpeg_calloc_align_inner")

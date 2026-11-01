/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/* From espos-p4-cockpit main/jlp/default_layout.h, changed for the M5Stack Tough (main/jlp/README.md). */
#pragma once

namespace jlp {

// Compiled-in fallback layout, shown when no layout has been saved (first
// boot, factory reset) and replaced by the first push from the designer.
// Sized for the Tough: 320x240 minus the 28 px status strip leaves 320x212.
// Read-only widgets only, so a fresh device never PUTs to the boat.
inline constexpr const char* kDefaultLayoutJson = R"json(
{
  "schema": 1,
  "name": "default",
  "screens": [
    {
      "id": "main",
      "title": "Main",
      "widgets": [
        {
          "type": "value", "id": "sog",
          "label": "SOG",
          "bind": "navigation.speedOverGround",
          "display": { "unit": "kn", "scale": 1.943844, "offset": 0.0, "decimals": 1 },
          "x": 6, "y": 6, "w": 151, "h": 96
        },
        {
          "type": "value", "id": "depth",
          "label": "DEPTH",
          "bind": "environment.depth.belowTransducer",
          "display": { "unit": "m", "scale": 1.0, "offset": 0.0, "decimals": 1 },
          "x": 163, "y": 6, "w": 151, "h": 96
        },
        {
          "type": "label", "id": "hint",
          "label": "Push a layout from the espOS HMI Designer",
          "x": 6, "y": 112, "w": 308, "h": 94
        }
      ]
    }
  ]
}
)json";

}  // namespace jlp

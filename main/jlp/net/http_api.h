/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/* From espos-p4-cockpit main/jlp/net/http_api.h, changed for the M5Stack Tough (main/jlp/README.md). */
#pragma once

#include <stdint.h>

#include "../features.h"

#if JLP_VOICE
namespace espos_voice {
class WyomingSatellite;
}
#endif

namespace jlp {

// Starts the layout-player HTTP API on `port`: POST /layout, POST /screen,
// GET /hello, /healthz, /screenshot, /beep.
void http_api_start(uint16_t port);

#if JLP_VOICE
// Register the Wyoming voice satellite so /hello can report its status.
// Null (the default) reports "unavailable".
void http_api_set_wyoming(espos_voice::WyomingSatellite* sat);
#endif

}  // namespace jlp

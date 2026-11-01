/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/* From espos-p4-cockpit main/jlp/net/sk_put.h, changed for the M5Stack Tough (main/jlp/README.md). */
#pragma once

#include <string>

namespace jlp {

// Typed helpers over espos_sk_put(): each serialises the value to JSON
// and logs the server's answer. espos_sk_put is thread-safe, so they
// are callable from any task.
void put_bool(const std::string& path, bool value);
void put_int(const std::string& path, int value);
void put_float(const std::string& path, float value);
void put_string(const std::string& path, const std::string& value);

// PUT an explicit JSON null to `path`. Used by button widgets whose
// value is null — e.g. raising an anchor by clearing
// navigation.anchor.position.
void put_null(const std::string& path);

// PUT a position object {latitude, longitude} to `path` — used to drop
// the anchor at the boat's current fix (navigation.anchor.position).
void put_position(const std::string& path, double latitude,
                  double longitude);

/** PUT a notification ACK to `notifications.<path_after_prefix>`.
 *  Sends the SK convention `{value: {state: "normal", method: [],
 *  message: ""}}`. Called by the alert overlay when the operator
 *  taps ACK on a pending notification. */
void put_notification_ack(const std::string& path_after_prefix);

}  // namespace jlp

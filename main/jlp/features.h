/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Which optional parts of the JSON Layout Player this board builds. The
 * player code comes from espos-p4-cockpit, where every part is present; the
 * switches keep those files close to the original instead of deleting code a
 * shared player will need again.
 *
 *   JLP_VOICE   the Wyoming voice satellite: `voice`, `speaker`, `mic` and
 *               `volume` widgets, /mic_probe*. Needs espos_voice and a mic.
 *   JLP_STREAM  the MJPEG `stream` widget. Needs the ESP32-P4 JPEG decoder.
 *
 * The Tough has neither (no esp-sr on the classic ESP32, no JPEG engine).
 */
#pragma once

#ifndef JLP_VOICE
#define JLP_VOICE 0
#endif

#ifndef JLP_STREAM
#define JLP_STREAM 0
#endif

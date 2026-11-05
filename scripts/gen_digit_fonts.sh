#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Thomas Christoffel
# SPDX-License-Identifier: Apache-2.0
#
# Generates main/jlp/fonts/lv_font_montserrat_digits_<size>.c: Montserrat
# Medium above LVGL's largest built-in size (48 px), for numbers only, so a
# value tile can fill the 320 px panel ("88.8" at 140 px). Letters fall back
# to the built-in 48 px font. Needs Node.js; the TTF is the one LVGL builds
# its own Montserrat fonts from.
#
#   scripts/gen_digit_fonts.sh path/to/Montserrat-Medium.ttf
set -euo pipefail

ttf=${1:?usage: $0 path/to/Montserrat-Medium.ttf}
out=$(cd "$(dirname "$0")/.." && pwd)/main/jlp/fonts
mkdir -p "$out"

# Space, + , - . digits :, and the em dash a value tile shows before data.
range=0x20,0x2B-0x2E,0x30-0x3A,0x2014

for size in 60 72 88 104 120 140; do
  name=lv_font_montserrat_digits_$size
  npx --yes lv_font_conv@1.5.3 --no-compress --no-prefilter --bpp 4 \
    --size "$size" --font "$ttf" -r "$range" --format lvgl \
    --force-fast-kern-format --lv-include lvgl.h --lv-font-name "$name" \
    --lv-fallback lv_font_montserrat_48 -o "$out/$name.c"
  # The header records the options; keep the local TTF path out of it.
  sed -i 's|--font [^ ]*Montserrat-Medium.ttf|--font Montserrat-Medium.ttf|' "$out/$name.c"
done

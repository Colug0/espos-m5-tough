/* SPDX-FileCopyrightText: 2026 Thomas Christoffel */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * espressif/esp_new_jpeg allocates its encoder working buffers through
 * jpeg_calloc_inner() and jpeg_calloc_align_inner(), which prefer internal
 * RAM (heap_caps_calloc_prefer / heap_caps_aligned_calloc with
 * MALLOC_CAP_INTERNAL first). On the Tough every /screenshot of the designer's
 * mirror mode then took over 35 KB of internal RAM for ~100 ms: free fell from
 * 53 to 13.6 KB with a 4.9 KB largest block, and espOS raised lowMemory
 * (measured 2026-11-01). These replacements, linked in with --wrap (see
 * CMakeLists.txt), prefer PSRAM like the library's own jpeg_calloc() and
 * jpeg_calloc_align() do. The library still frees with heap_caps_free(),
 * which takes memory from either pool.
 *
 * Signatures read from the library's machine code (esp_jpeg_memory.c.obj,
 * esp_new_jpeg 1.x): jpeg_calloc_inner(size), jpeg_calloc_align_inner(size,
 * alignment).
 */
#include <stddef.h>

#include "esp_heap_caps.h"

void *__wrap_jpeg_calloc_inner(size_t size)
{
    return heap_caps_calloc_prefer(1, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void *__wrap_jpeg_calloc_align_inner(size_t size, int aligned)
{
    void *p = heap_caps_aligned_calloc((size_t)aligned, 1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_aligned_calloc((size_t)aligned, 1, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

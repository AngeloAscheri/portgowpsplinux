/* "Smooth HUD" (rt_gpu.c): PPSSPP's copy of xBRZ (GPL-3.0, by Zenju) scales a picture by 2 to 6 without
 * blurring its edges. The renderer keeps RGBA bytes; xBRZ reads ARGB words, so red and blue trade places on
 * the way in and back out (they weigh differently in its colour distance). */
#include <stdint.h>
#include <vector>

#include "xbrz.h"

static uint32_t swap_rb(uint32_t c) { return (c & 0xFF00FF00u) | (c & 0xFFu) << 16 | (c >> 16 & 0xFFu); }

extern "C" void rt_xbrz_scale(int factor, const uint32_t *src, uint32_t *dst, int w, int h) {
    std::vector<uint32_t> in((size_t)w * h);
    for (size_t i = 0; i < in.size(); i++) in[i] = swap_rb(src[i]);
    xbrz::scale((size_t)factor, in.data(), dst, w, h, xbrz::ColorFormat::ARGB);
    for (size_t i = 0, n = (size_t)w * h * factor * factor; i < n; i++) dst[i] = swap_rb(dst[i]);
}

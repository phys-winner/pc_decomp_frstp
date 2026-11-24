#pragma once
/* Frostpunk's interface art at a phone's resolution, the same for both renderers.
 *
 * The interface art is PC 4K data: single-level BGRA8 atlases of 4096x4096 and pictures of 2562x2565,
 * 2751x2112, 4739x2612 ...  In a developed city they were 1.37 GB of the 2.18 GB of live game
 * textures on the Pixel 10a (8 GB, lmkd kills).  A phone panel is at most 1080 pixels high and the
 * interface is sampled with normalised coordinates, so such a texture is created with its longer edge
 * halved until it is <= debug.fp.ui_texture_limit (default 2048; 0 = off): a 2x2 box filter per
 * halving, 1/4 of the memory.  Only immutable data is touched: one level, one layer, one sample,
 * shader-resource binding only, no CPU access or misc flags, default or immutable usage, created with
 * initial data.
 *
 * Used by the DXVK bridge (android/dxvk/bridge.cpp, CreateTexture2D) and by the native Vulkan device
 * (android/vulkan/lib/com.cpp, CreateTexture2D), so both read the same game files into the same
 * textures (FP_UI_TEXTURE_SHRINK and RDVK_UI_TEXTURE_SHRINK lines).  Header only, C++. */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace fp_ui_texture {

/* debug.fp.ui_texture_limit: the longest edge kept (256..16384), 0 = off; read once. */
inline unsigned limit() {
    static const unsigned value = [] {
#ifdef __ANDROID__
        char text[PROP_VALUE_MAX] = {0};
        if (__system_property_get("debug.fp.ui_texture_limit", text) <= 0 || !text[0]) return 2048u;
        unsigned long v = strtoul(text, nullptr, 0);
        return v == 0 ? 0u : unsigned(std::max(256ul, std::min(v, 16384ul)));
#else
        return 2048u;
#endif
    }();
    return value;
}

/* D3D11 values: DXGI_FORMAT_R8G8B8A8_UNORM(_SRGB) 28/29, B8G8R8A8_UNORM(_SRGB) 87/91,
 * D3D11_BIND_SHADER_RESOURCE 8, D3D11_USAGE_DEFAULT 0 / IMMUTABLE 1. */
inline bool candidate(unsigned format, unsigned width, unsigned height, unsigned mips, unsigned layers, unsigned samples,
                      unsigned usage, unsigned bind, unsigned cpu, unsigned misc, unsigned lim) {
    if (format != 28 && format != 29 && format != 87 && format != 91) return false;
    if (mips != 1 || layers != 1 || samples != 1 || cpu || misc || bind != 8) return false;
    if (usage != 0 && usage != 1) return false;
    return lim != 0 && std::max(width, height) > lim;
}

/* One 2x2 box-filter halving of a 4-byte-per-pixel image (odd edges repeat their last row/column),
 * rounding to nearest. */
inline void halve(const uint8_t* in, size_t in_pitch, unsigned w, unsigned h, std::vector<uint8_t>& out, unsigned& nw, unsigned& nh) {
    nw = std::max(1u, w / 2);
    nh = std::max(1u, h / 2);
    out.assign(size_t(nw) * nh * 4, 0);
    for (unsigned y = 0; y < nh; ++y) {
        const uint8_t* r0 = in + size_t(std::min(2 * y, h - 1)) * in_pitch;
        const uint8_t* r1 = in + size_t(std::min(2 * y + 1, h - 1)) * in_pitch;
        uint8_t* o = out.data() + size_t(y) * nw * 4;
        for (unsigned x = 0; x < nw; ++x) {
            size_t a = size_t(std::min(2 * x, w - 1)) * 4, b = size_t(std::min(2 * x + 1, w - 1)) * 4;
            for (unsigned k = 0; k < 4; ++k) o[x * 4 + k] = uint8_t((unsigned(r0[a + k]) + r0[b + k] + r1[a + k] + r1[b + k] + 2) / 4);
        }
    }
}

/* Halves until the longer edge is <= lim (times == 0) or exactly `times` times; w/h become the new
 * size, out holds tightly packed rows (pitch w * 4).  Returns the number of halvings (0: out is left
 * as it was and w/h unchanged). */
inline unsigned shrink(const uint8_t* src, size_t pitch, unsigned& w, unsigned& h, unsigned lim, unsigned times, std::vector<uint8_t>& out) {
    std::vector<uint8_t> next;
    const uint8_t* in = src;
    size_t in_pitch = pitch;
    unsigned done = 0;
    while (times ? done < times : std::max(w, h) > lim) {
        unsigned nw, nh;
        halve(in, in_pitch, w, h, next, nw, nh);
        out.swap(next);
        in = out.data();
        in_pitch = size_t(nw) * 4;
        w = nw;
        h = nh;
        ++done;
    }
    return done;
}

}  // namespace fp_ui_texture

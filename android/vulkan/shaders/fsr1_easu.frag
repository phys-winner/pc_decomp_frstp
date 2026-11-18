#version 450
/* Present-time AMD FSR 1 EASU of the native Vulkan device (android/vulkan/lib/present.cpp): the
 * shader of android/dxvk/fsr1_present.frag (D3D11/DXVK path) without the gamma ramp, which the
 * native presenter never uses.  Output size in specialization constants 3 and 4, source rectangle
 * in the push constants. */
#extension GL_GOOGLE_include_directive : require

layout(constant_id = 3) const uint s_output_width = 1;
layout(constant_id = 4) const uint s_output_height = 1;
layout(set = 0, binding = 0) uniform sampler2D s_image;
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform present_info_t { ivec2 src_offset; uvec2 src_extent; };

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(s_image, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(s_image, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(s_image, p, 2); }
#include "ffx_fsr1.h"

void main() {
  AU4 c0, c1, c2, c3;
  FsrEasuCon(c0, c1, c2, c3, AF1(src_extent.x), AF1(src_extent.y),
    AF1(src_extent.x), AF1(src_extent.y), AF1(s_output_width), AF1(s_output_height));
  AF3 color;
  FsrEasuF(color, AU2(gl_FragCoord.xy), c0, c1, c2, c3);
  o_color = vec4(color, 1.0);
}

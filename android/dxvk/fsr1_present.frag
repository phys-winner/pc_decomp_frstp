#version 450
#extension GL_GOOGLE_include_directive : require

layout(constant_id = 1) const bool s_gamma_bound = false;
layout(constant_id = 3) const uint s_output_width = 1;
layout(constant_id = 4) const uint s_output_height = 1;
layout(binding = 0) uniform sampler2D s_image;
layout(binding = 1) uniform sampler1D s_gamma;
layout(location = 0) in vec2 i_coord;
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
  if (s_gamma_bound) {
    o_color = vec4(texture(s_gamma, o_color.r).r, texture(s_gamma, o_color.g).g,
      texture(s_gamma, o_color.b).b, o_color.a);
  }
}

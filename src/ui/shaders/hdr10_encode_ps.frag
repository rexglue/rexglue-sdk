// HDR10 output (display_hdr): turns the painted SDR picture into the HDR10
// signal (BT.2020 primaries, SMPTE ST 2084 "PQ" brightness) for a swapchain
// with VK_COLOR_SPACE_HDR10_ST2084_EXT.
//
// The canvas holds what the SDR swapchain would have shown: the guest output
// (after the game's gamma ramp) and the UI over it, as display-encoded values.
// Its alpha is the UI's coverage (the canvas is cleared to alpha 0, the guest
// output writes only RGB, and the UI drawers add their alpha), so the UI stays
// at paper white while the game's own highlights may go brighter.
//
//   1. decode with the SDR display's 2.2 power curve (what the picture is
//      graded for on a PC display);
//   2. BT.709 -> BT.2020 primaries (ITU-R BT.2087), linear;
//   3. highlights only ("subtle" inverse tone mapping, on luminance so hues
//      keep): below the knee nothing changes; above it the brightest SDR
//      values ease up to `peak` (t^power keeps the curve flat at the knee);
//   4. scale so SDR white is `paper_white` nits, then PQ-encode.
//
// glslangValidator -V --target-env vulkan1.0 --vn hdr10_encode_ps
//   -o vulkan_spirv/hdr10_encode_ps.h hdr10_encode_ps.frag

#version 450
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 0, binding = 0) uniform texture2D canvas;

layout(push_constant) uniform Constants {
  // Offset 0 is the vertex shader's rectangle (guest_output_triangle_strip_rect_vs).
  layout(offset = 16) float paper_white;  // nits: SDR white
  float peak;                             // nits: the brightest highlight
  float knee;                             // linear (1 = SDR white): highlights start
  float power;                            // ease-in of the highlight curve
} c;

layout(location = 0) out vec4 out_color;

void main() {
  vec4 s = texelFetch(canvas, ivec2(gl_FragCoord.xy), 0);
  vec3 encoded = clamp(s.rgb, 0.0, 1.0);
  float ui_coverage = clamp(s.a, 0.0, 1.0);

  vec3 linear709 = pow(encoded, vec3(2.2));
  // Columns: the BT.2020 contribution of BT.709 R, G and B.
  const mat3 bt709_to_bt2020 = mat3(
      0.627403896, 0.069097289, 0.016391439,
      0.329283039, 0.919540395, 0.088013308,
      0.043313065, 0.011362316, 0.895595253);
  vec3 linear2020 = bt709_to_bt2020 * linear709;

  float luminance = dot(linear2020, vec3(0.2627, 0.6780, 0.0593));
  float ratio = max(c.peak / max(c.paper_white, 1.0), 1.0);
  float scale = 1.0;
  if (luminance > c.knee && ratio > 1.0) {
    float t = clamp((luminance - c.knee) / max(1.0 - c.knee, 1.0e-4), 0.0, 1.0);
    scale = 1.0 + (ratio - 1.0) * pow(t, c.power) / luminance;
  }
  scale = mix(scale, 1.0, ui_coverage);

  vec3 nits = linear2020 * (scale * c.paper_white);
  vec3 l = clamp(nits * (1.0 / 10000.0), 0.0, 1.0);
  const float m1 = 0.1593017578125;
  const float m2 = 78.84375;
  const float c1 = 0.8359375;
  const float c2 = 18.8515625;
  const float c3 = 18.6875;
  vec3 lp = pow(l, vec3(m1));
  out_color = vec4(pow((c1 + c2 * lp) / (1.0 + c3 * lp), vec3(m2)), 1.0);
}

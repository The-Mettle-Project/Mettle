#version 330 core
// MythicUI GL renderer (src/mythic_render_gl.mettle): one instanced quad per draw item, covering
// the integer device-px box a_box. Kept to the GLES 3.0 subset.
layout(location = 0) in ivec4 a_box;    // x0, y0, x1, y1 of the quad
layout(location = 1) in ivec4 a_cbox;   // the command's box (rounded-clip test, as the CPU does)
layout(location = 2) in vec4 a_a;
layout(location = 3) in vec4 a_b;
layout(location = 4) in vec4 a_c;
layout(location = 5) in vec4 a_d;
layout(location = 6) in uvec4 a_info;   // kind, color, color2, clip shapes (first << 8 | count)
uniform vec2 u_size;
flat out ivec4 v_cbox;
flat out vec4 v_a;
flat out vec4 v_b;
flat out vec4 v_c;
flat out vec4 v_d;
flat out uvec4 v_info;
void main() {
  int k = gl_VertexID;
  float x = float((k & 1) == 0 ? a_box.x : a_box.z);
  float y = float((k & 2) == 0 ? a_box.y : a_box.w);
  gl_Position = vec4(x / u_size.x * 2.0 - 1.0, 1.0 - y / u_size.y * 2.0, 0.0, 1.0);
  v_cbox = a_cbox;
  v_a = a_a; v_b = a_b; v_c = a_c; v_d = a_d;
  v_info = a_info;
}

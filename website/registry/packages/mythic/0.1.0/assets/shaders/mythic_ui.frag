#version 330 core
// MythicUI GL renderer, fragment stage. Every formula follows the CPU rasterizer
// (src/mythic_raster*.mettle) step by step, integer rounding included, so the output matches the
// CPU goldens. Output is premultiplied; blending is ONE, ONE_MINUS_SRC_ALPHA in plain sRGB space.
// Kept to the GLES 3.0 subset.
//
// kind      | a                   | b             | c                    | d
// 1 FILL    | shape x0 y0 x1 y1   | radii tl tr br bl
// 2 STROKE  | outer shape         | outer radii   | inner shape          | inner radii
// 3 SHADOW  | shape (1/4 px grid) | radii         | sigma
// 4 GRAD    | shape               | radii         | x0 y0 dx/l2 dy/l2    |      (colors premultiplied)
// 5 GLYPH   | atlas dx dy page darken
// 6 IMAGE   | shape               | radii         | ix iy isx isy        | opacity
// 7 PATH    | sprite dx dy        (u_paths, the sprite atlas)
// 8 PATH    | band dx dy          (u_image, the band texture of an oversized path)
uniform vec2 u_size;
uniform sampler2DArray u_glyphs;
uniform sampler2D u_paths;
uniform sampler2D u_image;
uniform sampler2D u_clips;      // 2 texels per rounded clip shape: x0 y0 x1 y1, radius
uniform vec4 u_gamma[64];       // small-text darkening LUT (256 entries)
flat in ivec4 v_cbox;
flat in vec4 v_a;
flat in vec4 v_b;
flat in vec4 v_c;
flat in vec4 v_d;
flat in uvec4 v_info;
out vec4 o_color;

const int BAYER[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);

float corner_cov(float cx, float cy, float ccx, float ccy, float r) {
  float dx = cx - ccx;
  float dy = cy - ccy;
  return clamp(0.5 - (sqrt(dx * dx + dy * dy) - r), 0.0, 1.0);
}

// Box-filter area along straight edges, clamp(0.5 - d) inside the corner boxes (rr_cov).
float rr_cov(vec4 s, vec4 r, vec2 p) {
  float cx = p.x + 0.5;
  float cy = p.y + 0.5;
  if (cy < s.y + r.x && cx < s.x + r.x) { return corner_cov(cx, cy, s.x + r.x, s.y + r.x, r.x); }
  if (cy < s.y + r.y && cx > s.z - r.y) { return corner_cov(cx, cy, s.z - r.y, s.y + r.y, r.y); }
  if (cy > s.w - r.z && cx > s.z - r.z) { return corner_cov(cx, cy, s.z - r.z, s.w - r.z, r.z); }
  if (cy > s.w - r.w && cx < s.x + r.w) { return corner_cov(cx, cy, s.x + r.w, s.w - r.w, r.w); }
  float cvx = clamp(min(p.x + 1.0, s.z) - max(p.x, s.x), 0.0, 1.0);
  float cvy = clamp(min(p.y + 1.0, s.w) - max(p.y, s.y), 0.0, 1.0);
  return cvx * cvy;
}

// ---- shadows (mythic_raster_shadow): gaussian of a rounded rect, sigma = blur / 2

float erf_as(float x) {     // Abramowitz-Stegun 7.1.26, as mythic_erff
  float ax = abs(x);
  float t = 1.0 / (1.0 + 0.3275911 * ax);
  float poly = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741 + t * (-1.453152027 + t * 1.061405429))));
  float y = 1.0 - poly * exp(-ax * ax);
  return x < 0.0 ? -y : y;
}

bool row_extent(vec4 s, vec4 r, float y, out float xl, out float xr) {
  xl = s.x;
  xr = s.z;
  if (y <= s.y || y >= s.w) { return false; }
  if (y < s.y + r.x) {
    float d = s.y + r.x - y;
    xl = s.x + r.x - sqrt(max(r.x * r.x - d * d, 0.0));
  } else if (y > s.w - r.w) {
    float d = y - (s.w - r.w);
    xl = s.x + r.w - sqrt(max(r.w * r.w - d * d, 0.0));
  }
  if (y < s.y + r.y) {
    float d = s.y + r.y - y;
    xr = s.z - r.y + sqrt(max(r.y * r.y - d * d, 0.0));
  } else if (y > s.w - r.z) {
    float d = y - (s.w - r.z);
    xr = s.z - r.z + sqrt(max(r.z * r.z - d * d, 0.0));
  }
  return xr > xl;
}

// erf along x of each shape row, the gaussian sampled along y (mythic_shadow_eval)
float shadow_eval(vec4 s, vec4 r, float sigma, float px, float py) {
  float ext = 3.0 * sigma;
  float ya = max(py - ext, s.y);
  float yb = min(py + ext, s.w);
  if (yb <= ya) { return 0.0; }
  float stp = (yb - ya) / 24.0;
  float inv = 1.0 / (sigma * 1.4142135623730951);
  float k2 = -1.0 / (2.0 * sigma * sigma);
  float sum = 0.0;
  for (int i = 0; i < 24; i++) {
    float y = ya + (float(i) + 0.5) * stp;
    float xl;
    float xr;
    if (!row_extent(s, r, y, xl, xr)) { continue; }
    float t = y - py;
    sum += exp(t * t * k2) * 0.5 * (erf_as((px - xl) * inv) - erf_as((px - xr) * inv));
  }
  return clamp(sum * stp / (sigma * 2.5066282746310002 * 0.9973002), 0.0, 1.0);
}

float sh_prof(int k, int m, float frac, float inv) {    // edge profile table entry
  if (k < 0) { return 0.0; }
  if (k >= 2 * m) { return 1.0; }
  return 0.5 * (1.0 + erf_as((float(k - m) + 0.5 - frac) * inv));
}

float sh_corner(int i, int j, int m, float r, float fx, float fy, float sigma) {   // corner table entry
  float u = float(i - m) + 0.5 - fx;
  float v = float(j - m) + 0.5 - fy;
  return shadow_eval(vec4(0.0, 0.0, 1.0e6, 1.0e6), vec4(r, 0.0, 0.0, 0.0), sigma, u, v);
}

int fq(float v) { return int(floor((v - floor(v)) * 4.0 + 0.5)) & 3; }

// The CPU nine-slice evaluated per pixel: corner tiles times the opposite edge profiles, and the
// product of the four edge profiles elsewhere; shapes too small for it are evaluated directly.
float shadow_alpha(vec2 p) {
  vec4 s = v_a;
  float sigma = v_c.x;
  int px = int(p.x);
  int py = int(p.y);
  int m = int(ceil(3.0 * sigma)) + 1;
  vec4 q = floor(v_b * 4.0 + 0.5) * 0.25;
  ivec4 S = ivec4(m) + ivec4(ceil(q + 3.0 * sigma)) + 1;
  int gl0 = int(floor(s.x)) - m;
  int gr = int(ceil(s.z)) + m - 1;
  int gt = int(floor(s.y)) - m;
  int gb = int(ceil(s.w)) + m - 1;
  if (px < gl0 || px > gr || py < gt || py > gb) { return 0.0; }
  bool ok = gl0 + S.x <= gr + 1 - S.y && gl0 + S.w <= gr + 1 - S.z && gt + S.x <= gb + 1 - S.w && gt + S.y <= gb + 1 - S.z;
  if (!ok) { return shadow_eval(s, v_b, sigma, p.x + 0.5, p.y + 0.5); }
  int il = px - gl0;
  int ir = gr - px;
  int jt = py - gt;
  int jb = gb - py;
  float inv = 1.0 / (sigma * 1.41421356);
  float fxl = float(fq(s.x)) * 0.25;
  float fxr = float((4 - fq(s.z)) & 3) * 0.25;
  float fyt = float(fq(s.y)) * 0.25;
  float fyb = float((4 - fq(s.w)) & 3) * 0.25;
  float cl = sh_prof(il, m, fxl, inv);
  float cr = sh_prof(ir, m, fxr, inv);
  float rt = sh_prof(jt, m, fyt, inv);
  float rb = sh_prof(jb, m, fyb, inv);
  if (jt < S.x && il < S.x) { return sh_corner(il, jt, m, q.x, fxl, fyt, sigma) * cr * rb; }
  if (jt < S.y && ir < S.y) { return sh_corner(ir, jt, m, q.y, fxr, fyt, sigma) * cl * rb; }
  if (jb < S.z && ir < S.z) { return sh_corner(ir, jb, m, q.z, fxr, fyb, sigma) * cl * rt; }
  if (jb < S.w && il < S.w) { return sh_corner(il, jb, m, q.w, fxl, fyb, sigma) * cr * rt; }
  return cl * cr * rt * rb;
}

// ---- colors

ivec4 argb(uint c) {
  return ivec4(int(c >> 24u), int((c >> 16u) & 255u), int((c >> 8u) & 255u), int(c & 255u));
}

ivec4 texel_argb(ivec2 q) {
  vec4 t = texelFetch(u_image, q, 0);
  return ivec4(t.a * 255.0 + 0.5, t.r * 255.0 + 0.5, t.g * 255.0 + 0.5, t.b * 255.0 + 0.5);
}

ivec4 bilerp(ivec4 a, ivec4 b, int f) { return (a * (256 - f) + b * f) >> 8; }

ivec4 gradient_px(vec2 p) {
  float t = clamp((p.x + 0.5 - v_c.x) * v_c.z + (p.y + 0.5 - v_c.y) * v_c.w, 0.0, 1.0);
  float d = ((float(BAYER[(int(p.y) & 3) * 4 + (int(p.x) & 3)]) + 0.5) / 16.0 - 0.5) + 0.5;
  vec4 c0 = vec4(argb(v_info.y));
  vec4 c1 = vec4(argb(v_info.z));
  ivec4 v = ivec4(c0 + (c1 - c0) * t + d);
  int a = clamp(v.x, 0, 255);
  return ivec4(a, clamp(v.y, 0, a), clamp(v.z, 0, a), clamp(v.w, 0, a));
}

ivec4 image_px(vec2 p) {     // bilinear, edge clamped, premultiplied (shade_px)
  ivec2 sz = textureSize(u_image, 0);
  float sx = v_c.x + (p.x + 0.5) * v_c.z - 0.5;
  float sy = v_c.y + (p.y + 0.5) * v_c.w - 0.5;
  float fx0 = floor(sx);
  float fy0 = floor(sy);
  int wx = int((sx - fx0) * 256.0);
  int wy = int((sy - fy0) * 256.0);
  int x0 = int(fx0);
  int y0 = int(fy0);
  int xa = clamp(x0, 0, sz.x - 1);
  int xb = clamp(x0 + 1, 0, sz.x - 1);
  int ya = clamp(y0, 0, sz.y - 1);
  int yb = clamp(y0 + 1, 0, sz.y - 1);
  ivec4 top = bilerp(texel_argb(ivec2(xa, ya)), texel_argb(ivec2(xb, ya)), wx);
  ivec4 bot = bilerp(texel_argb(ivec2(xa, yb)), texel_argb(ivec2(xb, yb)), wx);
  return bilerp(top, bot, wy);
}

int mul255(int c, int a) {
  int t = c * a + 128;
  return (t + (t >> 8)) >> 8;
}

// Rounded clips: the CPU draws a command whose box (cut to the tile) reaches a corner of a clip
// shape into a layer and composites it through the product of the shapes' coverage.
bool box_hits_corners(vec4 s, float r, vec4 b) {
  if (b.x < s.x + r && b.y < s.y + r) { return true; }
  if (b.z > s.z - r && b.y < s.y + r) { return true; }
  if (b.z > s.z - r && b.w > s.w - r) { return true; }
  if (b.x < s.x + r && b.w > s.w - r) { return true; }
  return false;
}

int clip_mask(vec2 p) {      // -1 = not masked, else k in 0..256
  int n = int(v_info.w & 255u);
  if (n == 0) { return -1; }
  int first = int(v_info.w >> 8u);
  ivec2 tile = ivec2(p) / 64 * 64;
  vec4 b = vec4(max(v_cbox.xy, tile), min(v_cbox.zw, tile + 64));
  bool masked = false;
  float m = 1.0;
  for (int i = 0; i < n; i++) {
    int k = (first + i) * 2;
    vec4 s = texelFetch(u_clips, ivec2(k & 1023, k >> 10), 0);
    float r = texelFetch(u_clips, ivec2((k + 1) & 1023, (k + 1) >> 10), 0).x;
    if (box_hits_corners(s, r, b)) { masked = true; }
    m *= rr_cov(s, vec4(r), p);
  }
  if (!masked) { return -1; }
  return int(m * 256.0 + 0.5);
}

void main() {
  vec2 p = vec2(floor(gl_FragCoord.x), u_size.y - 1.0 - floor(gl_FragCoord.y));
  int kind = int(v_info.x);
  ivec4 col = argb(v_info.y);
  int km = clip_mask(p);
  if (kind == 4 || kind == 6) {
    // per-pixel premultiplied source scaled by coverage (draw_rrect_shaded, blend_pm)
    float cov = rr_cov(v_a, v_b, p);
    if (cov <= 0.0) { discard; }
    ivec4 sp;
    float op = 1.0;
    if (kind == 4) { sp = gradient_px(p); } else { sp = image_px(p); op = v_d.x; }
    int k = int(cov * op * 256.0 + 0.5);
    if (k == 0) { discard; }
    ivec4 s1 = (sp * k) >> 8;
    if (km >= 0) { s1 = (s1 * km) >> 8; }
    o_color = vec4(s1.yzw, s1.x) / 255.0;
    return;
  }
  int a = 0;
  float ca = float(col.x);
  if (kind == 1 || kind == 2) {
    float cov = rr_cov(v_a, v_b, p);
    if (kind == 2) { cov -= rr_cov(v_c, v_d, p); }
    if (cov <= 0.0) { discard; }
    a = int(ca * cov + 0.5);
  } else if (kind == 3) {
    a = int(ca * shadow_alpha(p) + 0.5);
  } else if (kind == 5) {
    ivec3 t = ivec3(int(p.x) + int(v_a.x), int(p.y) + int(v_a.y), int(v_a.z));
    int cv = int(texelFetch(u_glyphs, t, 0).r * 255.0 + 0.5);
    if (cv == 0) { discard; }
    if (v_a.w != 0.0) { cv = int(u_gamma[cv >> 2][cv & 3]); }
    a = (cv * col.x + 127) / 255;
  } else if (kind == 7 || kind == 8) {
    ivec2 t = ivec2(int(p.x) + int(v_a.x), int(p.y) + int(v_a.y));
    float v = kind == 7 ? texelFetch(u_paths, t, 0).r : texelFetch(u_image, t, 0).r;
    if (v <= 0.0) { discard; }
    a = int(ca * v + 0.5);
  }
  if (a <= 0) { discard; }
  a = min(a, 255);
  if (km >= 0) {
    // the CPU layer pixel (straight color over transparent), then scaled by the mask
    ivec4 s1 = ivec4(mul255(255, a), mul255(col.y, a), mul255(col.z, a), mul255(col.w, a));
    s1 = (s1 * km) >> 8;
    o_color = vec4(s1.yzw, s1.x) / 255.0;
    return;
  }
  float fa = float(a) / 255.0;
  o_color = vec4(vec3(col.yzw) / 255.0 * fa, fa);
}

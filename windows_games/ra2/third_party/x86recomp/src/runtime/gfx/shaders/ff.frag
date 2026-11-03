#version 450
// The SDL_GPU backend's fixed-function fragment stage: D3D's texture-stage
// cascade, alpha test and fog, driven by uniforms (see ff.vert).
//
// The uniform layout here and FsUniforms in gpu_device.cpp must agree.
// Bindings follow SDL_GPU's SPIR-V conventions for fragment shaders:
// sampled textures in set 2, uniform buffers in set 3.

layout(std140, set = 3, binding = 0) uniform FsUniforms {
  ivec4 color[8];   // per stage: COLOROP, COLORARG1, COLORARG2, COLORARG0
  ivec4 alpha[8];   // per stage: ALPHAOP, ALPHAARG1, ALPHAARG2, ALPHAARG0
  ivec4 stage[8];   // per stage: RESULTARG, texture bound, projected divisor index (-1 none), -
  vec4 tfactor;
  vec4 fog_color;
  vec4 fog;         // start, end, density
  ivec4 mode;       // alpha test func (0 off), alpha ref, fog: 0 off / 1 vertex / 2 table, table mode
  ivec4 mode2;      // flat shading, specular enable, -, -
} u;

layout(set = 2, binding = 0) uniform sampler2D tx0;
layout(set = 2, binding = 1) uniform sampler2D tx1;
layout(set = 2, binding = 2) uniform sampler2D tx2;
layout(set = 2, binding = 3) uniform sampler2D tx3;
layout(set = 2, binding = 4) uniform sampler2D tx4;
layout(set = 2, binding = 5) uniform sampler2D tx5;
layout(set = 2, binding = 6) uniform sampler2D tx6;
layout(set = 2, binding = 7) uniform sampler2D tx7;

layout(location = 0) in vec4 i_diff;
layout(location = 1) in vec4 i_spec;
layout(location = 2) flat in vec4 i_diff_flat;
layout(location = 3) flat in vec4 i_spec_flat;
layout(location = 4) in vec4 i_t[8];
layout(location = 12) in float i_fog;

layout(location = 0) out vec4 o_color;

vec4 Arg(int a, vec4 diff, vec4 cur, vec4 tex, vec4 tf, vec4 spec, vec4 temp) {
  vec4 v;
  switch (a & 0xF) {
    case 0: v = diff; break;
    case 1: v = cur; break;
    case 2: v = tex; break;
    case 3: v = tf; break;
    case 4: v = spec; break;
    case 5: v = temp; break;
    default: v = vec4(0.0); break;
  }
  if ((a & 0x20) != 0) v = vec4(v.a);  // ALPHAREPLICATE
  if ((a & 0x10) != 0) v = 1.0 - v;    // COMPLEMENT
  return v;
}

vec4 Op(int op, vec4 a1, vec4 a2, vec4 a0, vec4 diff, vec4 tex, vec4 tf, vec4 cur) {
  switch (op) {
    case 2: return a1;
    case 3: return a2;
    case 4: return a1 * a2;
    case 5: return a1 * a2 * 2.0;
    case 6: return a1 * a2 * 4.0;
    case 7: return a1 + a2;
    case 8: return a1 + a2 - 0.5;
    case 9: return (a1 + a2 - 0.5) * 2.0;
    case 10: return a1 - a2;
    case 11: return a1 + a2 - a1 * a2;
    case 12: return mix(a2, a1, diff.a);
    case 13: return mix(a2, a1, tex.a);
    case 14: return mix(a2, a1, tf.a);
    case 15: return a1 + a2 * (1.0 - tex.a);
    case 16: return mix(a2, a1, cur.a);
    case 17: return a1;  // PREMODULATE: approximated
    case 18: return vec4(a1.rgb + a1.a * a2.rgb, a1.a);
    case 19: return vec4(a1.rgb * a2.rgb + a1.a, a1.a);
    case 20: return vec4((1.0 - a1.a) * a2.rgb + a1.rgb, a1.a);
    case 21: return vec4((1.0 - a1.rgb) * a2.rgb + a1.a, a1.a);
    case 24: { float d = clamp(4.0 * dot(a1.rgb - 0.5, a2.rgb - 0.5), 0.0, 1.0); return vec4(d); }
    case 25: return a0 + a1 * a2;
    case 26: return mix(a2, a1, a0);
    default: return a1;  // bump mapping: not modelled
  }
}

bool AlphaPass(int func, float a, int ref) {
  int v = int(roundEven(clamp(a, 0.0, 1.0) * 255.0));
  switch (func) {
    case 1: return false;
    case 2: return v < ref;
    case 3: return v == ref;
    case 4: return v <= ref;
    case 5: return v > ref;
    case 6: return v != ref;
    case 7: return v >= ref;
    default: return true;
  }
}

float FogFactor(int mode, float d, vec4 fog) {
  if (mode == 1) return exp(-d * fog.z);
  if (mode == 2) { float x = d * fog.z; return exp(-x * x); }
  if (mode == 3) return fog.y == fog.x ? (d < fog.y ? 1.0 : 0.0) : (fog.y - d) / (fog.y - fog.x);
  return 1.0;
}

void main() {
  vec4 diff = u.mode2.x != 0 ? i_diff_flat : i_diff;
  vec4 spec = u.mode2.x != 0 ? i_spec_flat : i_spec;
  vec4 cur = diff, temp = vec4(0.0);
  for (int s = 0; s < 8; s++) {
    int cop = u.color[s].x;
    if (cop == 1) break;  // DISABLE ends the cascade
    vec4 tc = i_t[s];
    int proj = u.stage[s].z;
    vec2 uv = proj >= 0 ? tc.xy / tc[proj] : tc.xy;
    vec4 tex = vec4(0.0, 0.0, 0.0, 1.0);
    if (u.stage[s].y != 0) {
      switch (s) {
        case 0: tex = texture(tx0, uv); break;
        case 1: tex = texture(tx1, uv); break;
        case 2: tex = texture(tx2, uv); break;
        case 3: tex = texture(tx3, uv); break;
        case 4: tex = texture(tx4, uv); break;
        case 5: tex = texture(tx5, uv); break;
        case 6: tex = texture(tx6, uv); break;
        default: tex = texture(tx7, uv); break;
      }
    }
    ivec4 c = u.color[s], a = u.alpha[s];
    vec4 c1 = Arg(c.y, diff, cur, tex, u.tfactor, spec, temp);
    vec4 c2 = Arg(c.z, diff, cur, tex, u.tfactor, spec, temp);
    vec4 c0 = Arg(c.w, diff, cur, tex, u.tfactor, spec, temp);
    vec3 rgb = clamp(Op(cop, c1, c2, c0, diff, tex, u.tfactor, cur).rgb, 0.0, 1.0);
    float al = cur.a;
    if (a.x != 1) {
      vec4 a1 = Arg(a.y, diff, cur, tex, u.tfactor, spec, temp);
      vec4 a2 = Arg(a.z, diff, cur, tex, u.tfactor, spec, temp);
      vec4 a0 = Arg(a.w, diff, cur, tex, u.tfactor, spec, temp);
      al = clamp(Op(a.x, a1, a2, a0, diff, tex, u.tfactor, cur).a, 0.0, 1.0);
    }
    if (cop == 24) al = rgb.r;  // DOTPRODUCT3 replicates into alpha too
    if (u.stage[s].x == 5) temp = vec4(rgb, al);
    else cur = vec4(rgb, al);
  }
  if (u.mode2.y != 0) cur.rgb = clamp(cur.rgb + spec.rgb, 0.0, 1.0);
  if (!AlphaPass(u.mode.x, cur.a, u.mode.y)) discard;
  if (u.mode.z != 0) {
    float f = u.mode.z == 2 ? FogFactor(u.mode.w, i_fog, u.fog) : i_fog;
    cur.rgb = mix(u.fog_color.rgb, cur.rgb, clamp(f, 0.0, 1.0));
  }
  o_color = cur;
}

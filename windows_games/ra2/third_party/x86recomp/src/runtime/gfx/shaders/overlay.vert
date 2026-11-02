#version 450
// Overlay quads (gfx/overlay.cpp): one textured quad per draw, a triangle
// strip of 4 whose corners come from the vertex index, placed by the
// uniform rectangle (clip space); no vertex buffer.
//
// Bindings follow SDL_GPU's SPIR-V conventions (uniform buffers in set 1);
// the Metal build (METAL defined) numbers its one uniform buffer 0.
#ifdef METAL
#define UNIFORMS_BINDING set = 0, binding = 0
#else
#define UNIFORMS_BINDING set = 1, binding = 0
#endif

layout(std140, UNIFORMS_BINDING) uniform Quad {
  vec4 rect;  // left, top, right, bottom in clip space
};

layout(location = 0) out vec2 uv;

void main() {
  vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
  uv = corner;
  gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);
}

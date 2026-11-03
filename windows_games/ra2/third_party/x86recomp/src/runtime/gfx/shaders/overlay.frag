#version 450
// Overlay quads (see overlay.vert): the image times a tint (its alpha
// fades the whole quad). SDL_GPU's fragment conventions: sampled textures
// in set 2, uniform buffers in set 3.

layout(std140, set = 3, binding = 0) uniform Tint {
  vec4 tint;
};
layout(set = 2, binding = 0) uniform sampler2D image;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() { color = texture(image, uv) * tint; }

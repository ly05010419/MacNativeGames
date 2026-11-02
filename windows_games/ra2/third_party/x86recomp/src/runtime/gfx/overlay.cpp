#include "runtime/gfx/overlay.h"

#include <cstring>

#include "runtime/log.h"

// kOverlayVert and kOverlayFrag, built from shaders/overlay.*: MSL source
// on macOS and iOS, SPIR-V elsewhere (see gpu_device.cpp's ff shaders).
#include "overlay_frag.h"
#include "overlay_vert.h"

namespace recomp::gfx {

Overlay::Overlay(SDL_GPUDevice* dev, SDL_GPUTextureFormat target) : dev_(dev) {
#if defined(__APPLE__)
  const SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_MSL;
  const char* entry = "main0";  // SPIRV-Cross's name for main
  const size_t vs_size = sizeof kOverlayVert - 1, fs_size = sizeof kOverlayFrag - 1;
#else
  const SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_SPIRV;
  const char* entry = "main";
  const size_t vs_size = sizeof kOverlayVert, fs_size = sizeof kOverlayFrag;
#endif
  SDL_GPUShaderCreateInfo vs{};
  vs.code = reinterpret_cast<const Uint8*>(kOverlayVert);
  vs.code_size = vs_size;
  vs.entrypoint = entry;
  vs.format = format;
  vs.stage = SDL_GPU_SHADERSTAGE_VERTEX;
  vs.num_uniform_buffers = 1;
  SDL_GPUShaderCreateInfo fs = vs;
  fs.code = reinterpret_cast<const Uint8*>(kOverlayFrag);
  fs.code_size = fs_size;
  fs.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
  fs.num_samplers = 1;
  SDL_GPUShader* v = SDL_CreateGPUShader(dev_, &vs);
  SDL_GPUShader* f = SDL_CreateGPUShader(dev_, &fs);
  if (!v || !f) FatalF("gfx: overlay shaders: %s", SDL_GetError());

  SDL_GPUColorTargetDescription ctd{};
  ctd.format = target;
  ctd.blend_state.enable_blend = true;
  ctd.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  ctd.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  ctd.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
  ctd.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
  ctd.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  ctd.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
  SDL_GPUGraphicsPipelineCreateInfo pi{};
  pi.vertex_shader = v;
  pi.fragment_shader = f;
  pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
  pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
  pi.target_info.color_target_descriptions = &ctd;
  pi.target_info.num_color_targets = 1;
  pipeline_ = SDL_CreateGPUGraphicsPipeline(dev_, &pi);
  SDL_ReleaseGPUShader(dev_, v);
  SDL_ReleaseGPUShader(dev_, f);
  if (!pipeline_) FatalF("gfx: overlay pipeline: %s", SDL_GetError());

  SDL_GPUSamplerCreateInfo si{};
  si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
  si.address_mode_u = si.address_mode_v = si.address_mode_w =
      SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  sampler_ = SDL_CreateGPUSampler(dev_, &si);
}

Overlay::~Overlay() {
  for (auto& [id, im] : images_) SDL_ReleaseGPUTexture(dev_, im.tex);
  if (sampler_) SDL_ReleaseGPUSampler(dev_, sampler_);
  if (pipeline_) SDL_ReleaseGPUGraphicsPipeline(dev_, pipeline_);
}

void Overlay::SetImage(int id, const uint32_t* rgba, int w, int h) {
  Image& im = images_[id];
  if (im.tex && (im.w != w || im.h != h)) {
    SDL_ReleaseGPUTexture(dev_, im.tex);
    im.tex = nullptr;
  }
  if (!im.tex) {
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = Uint32(w);
    ti.height = Uint32(h);
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    im.tex = SDL_CreateGPUTexture(dev_, &ti);
    if (!im.tex) FatalF("gfx: overlay texture %dx%d: %s", w, h, SDL_GetError());
    im.w = w;
    im.h = h;
  }
  const Uint32 size = Uint32(w) * Uint32(h) * 4;
  SDL_GPUTransferBufferCreateInfo bi{};
  bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  bi.size = size;
  SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev_, &bi);
  if (!tb) FatalF("gfx: overlay upload buffer: %s", SDL_GetError());
  std::memcpy(SDL_MapGPUTransferBuffer(dev_, tb, false), rgba, size);
  SDL_UnmapGPUTransferBuffer(dev_, tb);
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
  SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureTransferInfo src{};
  src.transfer_buffer = tb;
  SDL_GPUTextureRegion dst{};
  dst.texture = im.tex;
  dst.w = Uint32(w);
  dst.h = Uint32(h);
  dst.d = 1;
  SDL_UploadToGPUTexture(cp, &src, &dst, false);
  SDL_EndGPUCopyPass(cp);
  SDL_SubmitGPUCommandBuffer(cmd);
  SDL_ReleaseGPUTransferBuffer(dev_, tb);
}

void Overlay::Draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, uint32_t w, uint32_t h) {
  if (items_.empty() || !w || !h) return;
  SDL_GPUColorTargetInfo t{};
  t.texture = target;
  t.load_op = SDL_GPU_LOADOP_LOAD;
  t.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &t, 1, nullptr);
  SDL_BindGPUGraphicsPipeline(pass, pipeline_);
  for (const OverlayItem& it : items_) {
    auto im = images_.find(it.image);
    if (im == images_.end() || it.alpha <= 0) continue;
    // Pixels to clip space (y up).
    const float rect[4] = {it.x / float(w) * 2 - 1, 1 - it.y / float(h) * 2,
                           (it.x + it.w) / float(w) * 2 - 1, 1 - (it.y + it.h) / float(h) * 2};
    const float tint[4] = {1, 1, 1, it.alpha};
    SDL_PushGPUVertexUniformData(cmd, 0, rect, sizeof rect);
    SDL_PushGPUFragmentUniformData(cmd, 0, tint, sizeof tint);
    SDL_GPUTextureSamplerBinding b{im->second.tex, sampler_};
    SDL_BindGPUFragmentSamplers(pass, 0, &b, 1);
    SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
  }
  SDL_EndGPURenderPass(pass);
}

}  // namespace recomp::gfx

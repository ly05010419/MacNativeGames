#pragma once
// Images drawn over the presented picture, after the frame (SDL_GPU
// backend): on-screen controls and statistics. Each item is an RGBA image
// (straight alpha) placed in the drawable's pixels and faded by `alpha`.
// Images are uploaded when set; the list is drawn every presentation.

#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <vector>

#include "runtime/gfx/device.h"

namespace recomp::gfx {

class Overlay {
 public:
  Overlay(SDL_GPUDevice* dev, SDL_GPUTextureFormat target);
  ~Overlay();
  Overlay(const Overlay&) = delete;
  Overlay& operator=(const Overlay&) = delete;

  // Sets image `id` (any number the caller picks) to w x h RGBA pixels.
  void SetImage(int id, const uint32_t* rgba, int w, int h);
  void SetItems(std::vector<OverlayItem> items) { items_ = std::move(items); }
  // Draws the items onto `target` (w x h), in a render pass of its own.
  void Draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, uint32_t w, uint32_t h);

 private:
  struct Image {
    SDL_GPUTexture* tex = nullptr;
    int w = 0, h = 0;
  };
  SDL_GPUDevice* dev_;
  SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
  SDL_GPUSampler* sampler_ = nullptr;
  std::map<int, Image> images_;
  std::vector<OverlayItem> items_;
};

}  // namespace recomp::gfx

#pragma once
// Presents a finished frame of a w x h back buffer on `dev`, with the dev
// aids every graphics API shares (<ID>_SHOT_EVERY / _SHOT_FROM / _SHOT_DIR
// screenshots, <ID>_HITCH long-frame and <ID>_FPS_REPORT frame-rate
// reports), then pumps window events and ticks the watchdog.

#include <cstdint>
#include <memory>

#include "runtime/gfx/device.h"

namespace recomp::hle {

void PresentFrame(gfx::Device& dev, int w, int h);
// The <ID>_HITCH and <ID>_FPS_REPORT reports for one presented frame: by
// PresentFrame, and by APIs that present another way (DirectDraw's
// primary surface).
void NoteFrameTiming();

// The gfx::Device backend every graphics API uses: <ID>_GFX=d3d9 or gpu;
// by default d3d9 where the build has it, else SDL_GPU.
gfx::Backend GfxBackend();
// SDL_CreateWindow flags the backend needs on top-level windows.
uint64_t GfxWindowFlags();
// Creates the device for `w` on GfxBackend(); fatal if that fails.
std::unique_ptr<gfx::Device> CreateGfxDevice(SDL_Window* w);

}  // namespace recomp::hle

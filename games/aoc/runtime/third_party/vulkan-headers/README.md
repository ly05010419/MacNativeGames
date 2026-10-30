# Vulkan headers

The C headers from Khronos's Vulkan-Headers, version 1.4.363 (`VK_HEADER_VERSION 363`), unmodified:

- `include/vulkan/vk_platform.h` and `vulkan_core.h`, the core API and its extensions;
- `include/vulkan/vulkan_vi.h`, for `VK_NN_vi_surface` (the Switch's window surface);
- `include/vk_video/*.h`, which `vulkan_core.h` includes.

They are used by the host's Vulkan layer (`dm/gfx/vk/hle_vk.m`), which implements on Metal the Vulkan functions that Hades calls. License: Apache-2.0 or MIT, your choice (`LICENSE.md`).

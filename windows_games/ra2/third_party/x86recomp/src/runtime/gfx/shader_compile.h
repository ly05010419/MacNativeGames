#pragma once
// GLSL to the SDL_GPU backend's shader code at run time: SPIR-V (glslang)
// for Vulkan, and on macOS MSL source from that SPIR-V (SPIRV-Cross, with
// the binding numbers as Metal indices, as the build does for the
// fixed-function shaders).

#include <cstdint>
#include <string>
#include <vector>

namespace recomp::gfx {

enum class ShaderStage { Vertex, Fragment };

// False with the compiler's messages in *log.
bool GlslToSpirv(const std::string& glsl, ShaderStage stage, std::vector<uint32_t>* spirv,
                 std::string* log);

#if defined(__APPLE__)
bool SpirvToMsl(const std::vector<uint32_t>& spirv, std::string* msl, std::string* log);
#endif

}  // namespace recomp::gfx

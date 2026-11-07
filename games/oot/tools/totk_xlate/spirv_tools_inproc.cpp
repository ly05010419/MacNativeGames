// Analysis-only normalization using the pinned Khronos SPIRV-Tools SSA pass.
// The original SPIR-V still generates MSL. This copy feeds the range analyzer.
#include <spirv-tools/optimizer.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int oot_spirv_ssa(const uint32_t *words, size_t count, void **out, size_t *bytes)
{
    if (!out || !bytes) return 0;
    *out = nullptr; *bytes = 0;
    if (!words || count < 5 || count > 524288) return 0;
    try {
        spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_1);
        optimizer.RegisterPass(spvtools::CreateSSARewritePass());
        std::vector<uint32_t> normalized;
        if (!optimizer.Run(words, count, &normalized) || normalized.size() > 524288) return 0;
        void *copy = malloc(normalized.size() * sizeof(uint32_t));
        if (!copy) return 0;
        memcpy(copy, normalized.data(), normalized.size() * sizeof(uint32_t));
        *out = copy; *bytes = normalized.size() * sizeof(uint32_t); return 1;
    } catch (...) { return 0; }
}
extern "C" void oot_spirv_ssa_free(void *p) { free(p); }

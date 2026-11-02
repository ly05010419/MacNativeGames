#include "runtime/gfx/shader_compile.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <mutex>

#if defined(__APPLE__)
#include <spirv_cross/spirv_msl.hpp>
#endif

namespace recomp::gfx {

bool GlslToSpirv(const std::string& glsl, ShaderStage stage, std::vector<uint32_t>* spirv,
                 std::string* log) {
  static std::once_flag once;
  std::call_once(once, [] { glslang::InitializeProcess(); });
  EShLanguage lang = stage == ShaderStage::Vertex ? EShLangVertex : EShLangFragment;
  glslang::TShader sh(lang);
  const char* src = glsl.c_str();
  sh.setStrings(&src, 1);
  sh.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
  sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
  const EShMessages msg = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!sh.parse(GetDefaultResources(), 450, false, msg)) {
    *log = sh.getInfoLog();
    return false;
  }
  glslang::TProgram prog;
  prog.addShader(&sh);
  if (!prog.link(msg)) {
    *log = prog.getInfoLog();
    return false;
  }
  spirv->clear();
  glslang::GlslangToSpv(*prog.getIntermediate(lang), *spirv);
  return true;
}

#if defined(__APPLE__)
bool SpirvToMsl(const std::vector<uint32_t>& spirv, std::string* msl, std::string* log) {
  try {
    spirv_cross::CompilerMSL c(spirv);
    spirv_cross::CompilerMSL::Options o = c.get_msl_options();
    o.set_msl_version(2, 1);
    o.enable_decoration_binding = true;
    o.enable_clip_distance_user_varying = false;
    c.set_msl_options(o);
    *msl = c.compile();
  } catch (const std::exception& e) {
    *log = e.what();
    return false;
  }
  return true;
}
#endif

}  // namespace recomp::gfx

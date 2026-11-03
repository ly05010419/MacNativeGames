// recomp_hlslc: the HLSL translator on a file.
//
//   recomp_hlslc <file.hlsl> [-T profile] [-E entry] [-D name[=value]]...
//                [--prelude file] [-o out.glsl] [--reflect]
//   recomp_hlslc <file.vso|file.pso> [--prelude file] [-o out.glsl] [--reflect]
//
// Profile and entry default to the "// profile P, entry E" line the
// D3DXCompileShader source dump starts with. --prelude makes a complete
// GLSL source: "#version 450", a stage define (R_VERTEX_SHADER or
// R_PIXEL_SHADER), the prelude file, then the translation (see
// runtime/hlsl/hlsl.h). --reflect prints the constant table and the
// interface to stdout.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "runtime/hlsl/hlsl.h"

namespace {

bool ReadFile(const char* path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

const char* UsageName(uint32_t u) {
  static const char* const kNames[] = {
      "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
      "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE",
  };
  if (u < 14) return kNames[u];
  return u == recomp::hlsl::kVPos ? "VPOS" : u == recomp::hlsl::kVFace ? "VFACE" : "?";
}

int PrintUsage() {
  std::fprintf(stderr,
               "usage: recomp_hlslc <file.hlsl> [-T profile] [-E entry] [-D name[=value]]...\n"
               "                    [--prelude file] [-o out.glsl] [--reflect]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace recomp::hlsl;
  const char* in = nullptr;
  const char* out = nullptr;
  const char* prelude = nullptr;
  std::string profile, entry;
  std::vector<Define> defines;
  bool reflect = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (a == "-T") {
      if (const char* v = next()) profile = v;
    } else if (a == "-E") {
      if (const char* v = next()) entry = v;
    } else if (a == "-D") {
      const char* v = next();
      if (!v) return PrintUsage();
      std::string d = v;
      size_t eq = d.find('=');
      defines.push_back(eq == std::string::npos ? Define{d, ""} : Define{d.substr(0, eq), d.substr(eq + 1)});
    } else if (a == "-o") {
      out = next();
    } else if (a == "--prelude") {
      prelude = next();
    } else if (a == "--reflect") {
      reflect = true;
    } else if (a[0] == '-') {
      return PrintUsage();
    } else {
      in = argv[i];
    }
  }
  if (!in) return PrintUsage();
  std::string src;
  if (!ReadFile(in, &src)) {
    std::fprintf(stderr, "%s: cannot read\n", in);
    return 1;
  }
  // "// profile vs_3_0, entry main[, defines A=1 B]"
  if (src.rfind("// profile ", 0) == 0) {
    std::string line = src.substr(0, src.find('\n'));
    char p[32] = {}, e[256] = {};
    if (std::sscanf(line.c_str(), "// profile %31[^,], entry %255[^,\n]", p, e) == 2) {
      if (profile.empty()) profile = p;
      if (entry.empty()) entry = e;
    }
    size_t d = line.find(", defines ");
    if (d != std::string::npos) {
      std::istringstream ds(line.substr(d + 10));
      std::string def;
      while (ds >> def) {
        size_t eq = def.find('=');
        defines.push_back(eq == std::string::npos ? Define{def, ""}
                                                  : Define{def.substr(0, eq), def.substr(eq + 1)});
      }
    }
  }
  // Compiled shaders (.vso / .pso: D3D9 tokens, as <ID>_SHADER_DUMP
  // writes them) go through the bytecode translator.
  std::string ext = std::string(in).size() > 4 ? std::string(in).substr(std::string(in).size() - 4) : "";
  bool bytecode = ext == ".vso" || ext == ".pso";
  if (!bytecode && (profile.empty() || entry.empty())) return PrintUsage();

  Program prog;
  std::string errors;
  bool ok = bytecode ? TranslateBytecode(reinterpret_cast<const uint32_t*>(src.data()), src.size() / 4,
                                         &prog, &errors)
                     : Compile(src, defines, entry, profile, 0, &prog, &errors);
  if (!ok) {
    // fxc style: the file name for "memory".
    if (errors.rfind("memory", 0) == 0) errors.replace(0, 6, in);
    std::fprintf(stderr, "%s\n", errors.c_str());
    return 1;
  }
  std::string glsl = prog.glsl;
  if (prelude) {
    std::string pre;
    if (!ReadFile(prelude, &pre)) {
      std::fprintf(stderr, "%s: cannot read\n", prelude);
      return 1;
    }
    glsl = std::string("#version 450\n#define ") +
           (prog.stage == Stage::Vertex ? "R_VERTEX_SHADER" : "R_PIXEL_SHADER") + "\n" + pre +
           "\n" + glsl;
  }
  if (out) {
    std::ofstream f(out, std::ios::binary);
    f << glsl;
    if (!f) {
      std::fprintf(stderr, "%s: cannot write\n", out);
      return 1;
    }
  } else if (!reflect) {
    std::fwrite(glsl.data(), 1, glsl.size(), stdout);
  }
  if (reflect) {
    static const char* const kSets[] = {"b", "i", "c", "s"};
    std::printf("constants:\n");
    for (const auto& c : prog.constants)
      std::printf("  %-32s %s%u x%u  class %u type %u  %ux%u [%u]\n", c.name.c_str(),
                  kSets[uint32_t(c.set)], c.index, c.count, uint32_t(c.cls), uint32_t(c.type),
                  c.rows, c.columns, c.elements);
    std::printf("inputs:");
    for (size_t i = 0; i < prog.inputs.size(); i++)
      std::printf(" %zu=%s%u", i, UsageName(prog.inputs[i].usage), prog.inputs[i].index);
    std::printf("\nvaryings:");
    for (const auto& v : prog.varyings)
      std::printf(" %s%u@%d", UsageName(v.usage), v.index, VaryingLocation(v));
    std::printf("\nsamplers:");
    for (const auto& s : prog.samplers) std::printf(" s%u(type %u)", s.reg, uint32_t(s.type));
    std::printf("\ncolor outputs %X, depth %d, point size %d, float registers %u\n",
                prog.color_outputs, prog.writes_depth, prog.writes_point_size, prog.float_regs);
  }
  return 0;
}

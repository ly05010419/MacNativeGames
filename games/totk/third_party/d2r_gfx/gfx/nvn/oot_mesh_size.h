#ifndef OOT_MESH_SIZE_H
#define OOT_MESH_SIZE_H
#include <string.h>

/* One unmodified OOT mesh MSL unit crashes M1 Max's MeshProgramKey backend
 * (Metal 32023.921, llvm::MachineOperand::setReg). Identical MSL with -Os
 * builds the pipeline. Match the complete source, not a shader-name prefix:
 * untested translations and patched variants keep their normal options.
 * build_shader_libs.py reads this same constant for offline compilation.
 * See games/oot/docs/mesh-compiler-investigation.md. */
#define OOT_MESH_SIZE_SOURCE_SHA256 "6e0e9e6f1fe97825fba77b0c4187256c6015c02845a3ff1c543bd0a1d090766d"
#define OOT_MESH_SIZE_MACOS_MAJOR 27
static inline int oot_mesh_size_source(const char *sha256) {
    return sha256 && !strcmp(sha256, OOT_MESH_SIZE_SOURCE_SHA256);
}
#endif

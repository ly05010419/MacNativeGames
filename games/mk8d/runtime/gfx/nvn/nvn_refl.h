// Shader reflection json → fixed-length C arrays (A: PC chain reflection; B: reflection produced by translating the original Switch shaders, route = "B", output of tools/dmsh_build.py).
// Kinds added by B: SBUFP = storage buffer device pointer (NVN storage buffer slot); SUPPORT = Ryujinx auxiliary constants; C1 = the shader's own constant segment (c[1]);
//            DRV0 / DRV2 = NVN driver constant segments c[0] (storage buffer address table) / c[2] (texture and sampler handle table)
// PC shader reflection json → fixed-length C arrays (converted once when the shader is fetched; each draw only walks the arrays and does not touch NSDictionary / NSString).
// Unit test tests/t_refl.m computes the expectations by hand from real reflection.
#pragma once
#include <stdint.h>

enum { NVN_RB_CBUF = 1, NVN_RB_SAMP, NVN_RB_TEX, NVN_RB_IMG, NVN_RB_TBUF,
       NVN_RB_SBUFP, NVN_RB_SUPPORT, NVN_RB_C1, NVN_RB_DRV0, NVN_RB_DRV2, NVN_RB_SAMPT, NVN_RB_UBOP, NVN_RB_TEXU, NVN_RB_SAMPU, NVN_RB_SAMPI, NVN_RB_SBUFU, NVN_RB_TEXD, NVN_RB_SAMPD, NVN_RB_TEXA, NVN_RB_SAMPA, NVN_RB_GEOMETRY };
// TEXA / SAMPA: an array of n handles in the driver table c2 that the shader indexes at run time (dmsh_build.py c2_array): element k = the texture / sampler of the slot whose c2 word lies
// at byte off + 8k (combined, else separate); words outside both tables give the dummy. Luigi's Mansion 3's shadow cascades
// TEXD / SAMPD: Ryujinx's fallback for a texture handle it cannot trace to one slot (handle type 4, <stage>_<t|tb|s>_cb0_40000000): an array of n textures / samplers indexed by
// the handle's texture / sampler ID (capped at n - 1); the replay binds slot k at element k and puts slot indices in c2 (profile key tex_dyn_slots, Octopath Traveler 0)
// SAMPI: the sampler ID is a constant in the shader (reg = the ID; Ryujinx's SeparateConstantSamplerHandle: the handle ORs a constant sampler into a bound texture, Monster Hunter Rise)
// UBOP: device pointer of compute-stage uniform slot r (the Maxwell compute stage reads uniforms via the c0 address table + global memory)   // SAMPT: the sampler is taken from the combined handle of texture slot r (high bits = sampler ID)   // Binding kind (type in the json); IMG = writable texture (D3D u_r, NVN image slot r); TBUF = typed buffer (D3D t_r Buffer<T>, buffer texture in NVN texture slot r)
enum { NVN_DPF_FLOAT = 0, NVN_DPF_UINT, NVN_DPF_SINT, NVN_DPF_DEPTH };            // Dummy texture pixel class (determined by msl_type)
enum { NVN_CT_FLOAT = 0, NVN_CT_UINT, NVN_CT_INT };                               // Vertex input component type
// Bindings of one shader: NVN gives a stage 32 texture and 32 sampler slots beside its uniform and storage buffers and images. OCTOPATH TRAVELER II's 3D world map material
// (UiMM_WorldMap3D) has a pixel shader with 30 textures, 30 samplers, 3 uniform buffers and the c1 / support / driver segments, 66 in all, which the old limit of 64 refused
// (the traveler selection drew its map black)
#define NVN_REFL_MAXB 96
#define NVN_REFL_MAXARG 192   // argument buffer entries of one shader (an array binding takes n of them)
#define NVN_REFL_MAXV 32

typedef struct {
    uint8_t kind, rw;        // rw: storage buffer access is read_write / write / device
    uint8_t dtt, dpf;        // Dummy texture when the texture slot is unbound: MTLTextureType value, pixel class
    uint16_t reg, aid;       // D3D register number, index within the argument buffer
    uint16_t off;            // TEXU / SAMPU: byte offset of the handle within the uniform buffer; SBUFU: of the 64-bit storage buffer address
    uint8_t n;               // TEXD / SAMPD, and TEXU / SAMPU arrays (element k = the handle at byte 8k of the uniform buffer): array length (argument buffer entries aid .. aid + n - 1)
    uint8_t f1d;             // TEX: read by 1D integer fetches as 2D rows of 16384 texels (dmsh_build.py "fetch1d"): a buffer texture here is bound as such rows (nvn_mtl.m tbuf_as_2d)
    uint8_t atom;            // IMG: the shader runs image atomics on it (dmsh_build.py "atomic"): the texture must allow Metal's texture atomics (nvn_mtl.m dummy_img_atomic)
} nvn_rbind;
typedef struct {
    int nb, abi, nv;         // Binding count, argument buffer slot, vertex input count
    nvn_rbind b[NVN_REFL_MAXB];
    struct { uint8_t loc, ct, nvn, sh[3]; } v[NVN_REFL_MAXV];   // loc: Metal vertex attribute slot (DXBC input register); nvn: NVN vertex attribute number (the n of HLSL semantic ATTRn);
                                                                // sh[ct]: Metal attribute index of the input's shadow of that component type in the <key>_bv library (0xff = none; dmsh_vin.py)
    uint8_t has_vin;         // has a shadow-input library <key>_bv (nvn_mtl.m vin_fn)
    uint16_t tg[3];          // Compute thread group size (none = 0)
    uint8_t route_b, has_c1; // Route B; has a c1 constant segment
    uint8_t bindless;        // Has TEXU / SAMPU bindings (which texture is read is known only at replay time)
    uint8_t dyn;             // Has TEXD / SAMPD bindings: c2's texture and sampler tables hold slot indices for this shader
    uint8_t drv_read[3];     // B: reads driver segments c0 / c2 at runtime (indices 0, 2; gathered statically from the MSL at packaging time, tools/dmsh_build.py drv_reads)
    uint16_t c1_len;         // Length of the c1 constant segment (B; 0 / 256 / 512 / 768 across the whole library)
    // For the fully-covering-first-draw proof (dmsh_build.py raster_facts; missing field = not provable): vertex - position xyz taken as-is from attribute pos_attr (-1 = no), w is the constant 1 (pos_w1) or taken from the same attribute's .w;
    // fragment - may discard, writes sample_mask, bitmap of the color numbers written
    int8_t pos_attr; uint8_t pos_w1, may_discard, writes_smask, color_mask, color_known;   // color_known: the color list was recorded at packaging (color_mask can be trusted)
    // Merge B step 2 (readable-tile version <key>_bf, dmsh_fetch.py): fragment - nfc redirectable samples (the i-th: texture slot fc_reg, interpolated output fc_locn),
    // bitmap fc_mixed of texture slots with mixed use (these slots must not be redirected to read the tile), nfo outputs (each with color number fo_color); vertex - .xy of interpolated output locn is taken as-is from attribute uv_attr[locn] (-1 = no)
    uint8_t nfc, fc_reg[32], fc_locn[32], nfo, fo_color[4]; uint64_t fc_mixed[4]; int8_t uv_attr[32];   // fc_mixed: a bit per texture slot 0-255 (slots 128+ are separate texture binds, NVN_SEPTEX_SLOT0)
    // Vertex - .x / .y of interpolated output locn = fma(.x / .y of attribute uv_aff_attr[locn], scale, offset), where scale / offset reference a uniform: (slot << 8) | (vector << 2) | component
    int8_t uv_aff_attr[32]; uint16_t uv_aff[32][4];   // [sx, ox, sy, oy]
    uint8_t *c1;             // c1 constant segment as-is (B): c1_len bytes, malloc'd by nvn_refl_from_json (NULL without one). It was a 4 KB array; Hyrule Warriors: Age of Calamity has 6 KB segments (its soldiers' vertex shaders)
} nvn_refl;

#ifdef __OBJC__
#import <Foundation/Foundation.h>
// 0 on success; on failure returns non-0 and writes the reason into err (unknown binding type, fixed-length limit exceeded, missing field)
int nvn_refl_from_json(NSDictionary *j, nvn_refl *out, char *err, int errlen);
#endif

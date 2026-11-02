// 着色器反射 json → 定长 C 数组（A：PC 链反射；B：Switch 原版翻译的反射，route = "B"，tools/dmsh_build.py 出）。
// B 新增种类：SBUFP = 存储缓冲设备指针（NVN 存储缓冲槽）；SUPPORT = Ryujinx 辅助常量；C1 = 着色器自带常数段（c[1]）；
//            DRV0 / DRV2 = NVN 驱动常量段 c[0]（存储缓冲地址表）/ c[2]（贴图与采样器句柄表）
// PC 着色器反射 json → 定长 C 数组（取着色器时转一次；每次绘制只遍历数组，不碰 NSDictionary / NSString）。
// 单测 tests/t_refl.m 拿真实反射手算期望。
// Shader reflection json → fixed-length C arrays (A: PC chain reflection; B: reflection produced by translating the original Switch shaders, route = "B", output of tools/dmsh_build.py).
// Kinds added by B: SBUFP = storage buffer device pointer (NVN storage buffer slot); SUPPORT = Ryujinx auxiliary constants; C1 = the shader's own constant segment (c[1]);
//            DRV0 / DRV2 = NVN driver constant segments c[0] (storage buffer address table) / c[2] (texture and sampler handle table)
// PC shader reflection json → fixed-length C arrays (converted once when the shader is fetched; each draw only walks the arrays and does not touch NSDictionary / NSString).
// Unit test tests/t_refl.m computes the expectations by hand from real reflection.
#pragma once
#include <stdint.h>

enum { NVN_RB_CBUF = 1, NVN_RB_SAMP, NVN_RB_TEX, NVN_RB_IMG, NVN_RB_TBUF,
       NVN_RB_SBUFP, NVN_RB_SUPPORT, NVN_RB_C1, NVN_RB_DRV0, NVN_RB_DRV2, NVN_RB_SAMPT, NVN_RB_UBOP, NVN_RB_SBUFU };   // SBUFU（TOTK）：存储缓冲地址由着色器自己从用户 uniform 槽 reg 的偏移 off 处读出 | SBUFU (TOTK): the storage buffer address is loaded by the shader from byte offset off of user uniform slot reg   // UBOP：计算阶段 uniform 槽 r 的设备指针（Maxwell 计算阶段经 c0 地址表 + 全局内存读 uniform）   // SAMPT：采样器取自贴图槽 r 的合并句柄（高位 = 采样器号）   // 绑定种类（json 的 type）；IMG = 可写贴图（D3D u_r，NVN 图像槽 r）；TBUF = 类型化缓冲（D3D t_r Buffer<T>，NVN 贴图槽 r 的缓冲贴图） | UBOP: device pointer of compute-stage uniform slot r (the Maxwell compute stage reads uniforms via the c0 address table + global memory)   // SAMPT: the sampler is taken from the combined handle of texture slot r (high bits = sampler ID)   // Binding kind (type in the json); IMG = writable texture (D3D u_r, NVN image slot r); TBUF = typed buffer (D3D t_r Buffer<T>, buffer texture in NVN texture slot r)
enum { NVN_DPF_FLOAT = 0, NVN_DPF_UINT, NVN_DPF_SINT, NVN_DPF_DEPTH };            // 哑贴图像素类（由 msl_type 定） | Dummy texture pixel class (determined by msl_type)
enum { NVN_CT_FLOAT = 0, NVN_CT_UINT, NVN_CT_INT };                               // 顶点输入分量类型 | Vertex input component type
#define NVN_REFL_MAXB 160   /* Luigi's Mansion 3: a 32-element texture array expands to 64 bindings */
#define NVN_REFL_MAXV 32

typedef struct {
    uint8_t kind, rw;        // rw：存储缓冲 access 为 read_write / write / device | rw: storage buffer access is read_write / write / device
    uint8_t dtt, dpf;        // 贴图槽未绑时的哑贴图：MTLTextureType 数值、像素类 | Dummy texture when the texture slot is unbound: MTLTextureType value, pixel class
    uint16_t reg, aid;       // D3D 寄存器号、参数缓冲内序号 | D3D register number, index within the argument buffer
    uint16_t off;            // SBUFU：uniform 块内的指针字节偏移 | SBUFU: byte offset of the pointer inside the uniform block
} nvn_rbind;
typedef struct {
    int nb, abi, nv;         // 绑定数、参数缓冲槽、顶点输入数 | Binding count, argument buffer slot, vertex input count
    nvn_rbind b[NVN_REFL_MAXB];
    struct { uint8_t loc, ct, nvn; } v[NVN_REFL_MAXV];   // loc：Metal 顶点属性槽（DXBC 输入寄存器）；nvn：NVN 顶点属性号（HLSL 语义 ATTRn 的 n） | loc: Metal vertex attribute slot (DXBC input register); nvn: NVN vertex attribute number (the n of HLSL semantic ATTRn)
    uint16_t tg[3];          // 计算线程组大小（无 = 0） | Compute thread group size (none = 0)
    uint8_t route_b, has_c1; // B 路线；有 c1 常数段 | Route B; has a c1 constant segment
    uint8_t drv_read[3];     // B：运行时读驱动段 c0 / c2（下标 0、2；出包时由 MSL 静态统计，tools/dmsh_build.py drv_reads） | B: reads driver segments c0 / c2 at runtime (indices 0, 2; gathered statically from the MSL at packaging time, tools/dmsh_build.py drv_reads)
    uint16_t c1_len;         // c1 常数段长度（B；全库 0 / 256 / 512 / 768） | Length of the c1 constant segment (B; 0 / 256 / 512 / 768 across the whole library)
    // 全覆盖首绘证明用（dmsh_build.py raster_facts；缺字段 = 不可证）：顶点——位置 xyz 原样取自属性 pos_attr（-1 = 否），w 为常数 1（pos_w1）或取同属性 .w；
    // 片元——可能丢弃、写 sample_mask、写出的颜色号位图
    // For the fully-covering-first-draw proof (dmsh_build.py raster_facts; missing field = not provable): vertex - position xyz taken as-is from attribute pos_attr (-1 = no), w is the constant 1 (pos_w1) or taken from the same attribute's .w;
    // fragment - may discard, writes sample_mask, bitmap of the color numbers written
    int8_t pos_attr; uint8_t pos_w1, may_discard, writes_smask, color_mask;
    // 合并 B 第 2 步（可读 tile 版本 <键>_bf，dmsh_fetch.py）：片元——可改采样 nfc 次（第 i 次：贴图槽 fc_reg、插值输出 fc_locn）、
    // 混用的贴图槽位图 fc_mixed（这些槽不得改读 tile）、输出 nfo 个（各自的颜色号 fo_color）；顶点——插值输出 locn 的 .xy 原样取自属性 uv_attr[locn]（-1 = 否）
    // Merge B step 2 (readable-tile version <key>_bf, dmsh_fetch.py): fragment - nfc redirectable samples (the i-th: texture slot fc_reg, interpolated output fc_locn),
    // bitmap fc_mixed of texture slots with mixed use (these slots must not be redirected to read the tile), nfo outputs (each with color number fo_color); vertex - .xy of interpolated output locn is taken as-is from attribute uv_attr[locn] (-1 = no)
    uint8_t nfc, fc_reg[32], fc_locn[32], nfo, fo_color[4]; uint64_t fc_mixed[2]; int8_t uv_attr[32];
    // 顶点——插值输出 locn 的 .x / .y = fma(属性 uv_aff_attr[locn] 的 .x / .y, 缩放, 偏移)，缩放 / 偏移引用 uniform：(槽 << 8) | (向量 << 2) | 分量
    // Vertex - .x / .y of interpolated output locn = fma(.x / .y of attribute uv_aff_attr[locn], scale, offset), where scale / offset reference a uniform: (slot << 8) | (vector << 2) | component
    int8_t uv_aff_attr[32]; uint16_t uv_aff[32][4];   // [sx, ox, sy, oy]
    uint8_t c1[4096];        // c1 常数段原样（B） | c1 constant segment as-is (B)
} nvn_refl;

#ifdef __OBJC__
#import <Foundation/Foundation.h>
// 成功 0；失败返回非 0 并把原因写进 err（未知绑定类型、超定长上限、字段缺失）
// 0 on success; on failure returns non-0 and writes the reason into err (unknown binding type, fixed-length limit exceeded, missing field)
int nvn_refl_from_json(NSDictionary *j, nvn_refl *out, char *err, int errlen);
#endif

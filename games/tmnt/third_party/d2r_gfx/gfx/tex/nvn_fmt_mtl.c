// NVNformat（nvn.h 0x01–0x9C）→ Metal。表按 NVN 枚举顺序逐项写，未列的格式（含 0、越界）返回 0。
// 分量重映射：RGBX / RGB_DXT1 一类「无 alpha」格式 a 取 1；RGB 系 16 位打包格式交换 r/b；其余恒等。
// BC1–7 在 iOS 上需 GPU 支持 BC（MTLDevice.supportsBCTextureCompression，iOS 16.4+；A19 已支持，未在本表核设备能力）。
// NVNformat (nvn.h 0x01–0x9C) → Metal. The table is written entry by entry in NVN enum order; unlisted formats (including 0 and out of range) return 0.
// Component remapping: "no alpha" formats such as RGBX / RGB_DXT1 read a as 1; RGB-family 16-bit packed formats swap r/b; all others are identity.
// BC1–7 on iOS require GPU BC support (MTLDevice.supportsBCTextureCompression, iOS 16.4+; A19 supports it; device capability is not checked in this table).
#include "nvn_fmt_mtl.h"

enum {   // MTLPixelFormat
    R8Unorm = 10, R8Snorm = 12, R8Uint = 13, R8Sint = 14, R16Unorm = 20, R16Snorm = 22, R16Uint = 23, R16Sint = 24, R16Float = 25,
    RG8Unorm = 30, RG8Snorm = 32, RG8Uint = 33, RG8Sint = 34, B5G6R5Unorm = 40, A1BGR5Unorm = 41, ABGR4Unorm = 42, BGR5A1Unorm = 43,
    R32Uint = 53, R32Sint = 54, R32Float = 55, RG16Unorm = 60, RG16Snorm = 62, RG16Uint = 63, RG16Sint = 64, RG16Float = 65,
    RGBA8Unorm = 70, RGBA8Unorm_sRGB = 71, RGBA8Snorm = 72, RGBA8Uint = 73, RGBA8Sint = 74, BGRA8Unorm = 80, BGRA8Unorm_sRGB = 81,
    RGB10A2Unorm = 90, RGB10A2Uint = 91, RG11B10Float = 92, RGB9E5Float = 93, RG32Uint = 103, RG32Sint = 104, RG32Float = 105,
    RGBA16Unorm = 110, RGBA16Snorm = 112, RGBA16Uint = 113, RGBA16Sint = 114, RGBA16Float = 115, RGBA32Uint = 123, RGBA32Sint = 124,
    RGBA32Float = 125, BC1 = 130, BC1_sRGB = 131, BC2 = 132, BC2_sRGB = 133, BC3 = 134, BC3_sRGB = 135, BC4_U = 140, BC4_S = 141,
    BC5_U = 142, BC5_S = 143, BC6H_S = 150, BC6H_U = 151, BC7 = 152, BC7_sRGB = 153,
    ASTC_SRGB0 = 186, ASTC_LDR0 = 204,   // 4x4 起；7x? 不存在，故 8x5 起跳一号（见 astc_off） | starting at 4x4; 7x? does not exist, so one number is skipped from 8x5 on (see astc_off)
    Depth16Unorm = 250, Depth32Float = 252, Stencil8 = 253, Depth32Float_Stencil8 = 260,
};

typedef struct { uint16_t mtl; uint8_t kind, noalpha, rb; } ent;   // kind 1 精确 2 替代；noalpha：a 取 1；rb：交换 r/b | kind: 1 exact, 2 substitute; noalpha: a reads as 1; rb: swap r/b
#define E(m) {m, 1, 0, 0}
#define X(m) {m, 1, 1, 0}     // 精确，无 alpha | exact, no alpha
#define S(m) {m, 2, 0, 0}     // 替代 | substitute
#define SX(m) {m, 2, 1, 0}
#define N {0, 0, 0, 0}        // 无对应 | no counterpart
#define SR(m) {m, 2, 0, 1}    // 替代，交换 r/b | substitute, swap r/b
#define SXR(m) {m, 2, 1, 1}

static const ent T[0x9D] = {
    [0x01] = E(R8Unorm), E(R8Snorm), E(R8Uint), E(R8Sint),
    [0x05] = E(R16Float), E(R16Unorm), E(R16Snorm), E(R16Uint), E(R16Sint),
    [0x0A] = E(R32Float), E(R32Uint), E(R32Sint),
    [0x0D] = E(RG8Unorm), E(RG8Snorm), E(RG8Uint), E(RG8Sint),
    [0x11] = E(RG16Float), E(RG16Unorm), E(RG16Snorm), E(RG16Uint), E(RG16Sint),
    [0x16] = E(RG32Float), E(RG32Uint), E(RG32Sint),
    // 0x19–0x24 RGB8 / RGB16 / RGB32 三分量：Metal 无 3 分量像素格式 → 返回 0。
    // 处理：上传时逐像素扩成同类 4 分量（RGBA8 / RGBA16 / RGBA32，a = 1，整数类 a = 1 原值），按 RGBX 采样；
    //       作缓冲贴图（target BUFFER）时改由着色器按 3 分量手读。
    // 0x19–0x24 RGB8 / RGB16 / RGB32 three-component: Metal has no 3-component pixel formats → return 0.
    // Handling: on upload expand per pixel to the 4-component format of the same kind (RGBA8 / RGBA16 / RGBA32, a = 1; for integer kinds a = raw value 1), sampled as RGBX;
    //           when used as a buffer texture (target BUFFER), the shader reads the 3 components manually instead.
    [0x19] = N, N, N, N, N, N, N, N, N, N, N, N,
    [0x25] = E(RGBA8Unorm), E(RGBA8Snorm), E(RGBA8Uint), E(RGBA8Sint),
    [0x29] = E(RGBA16Float), E(RGBA16Unorm), E(RGBA16Snorm), E(RGBA16Uint), E(RGBA16Sint),
    [0x2E] = E(RGBA32Float), E(RGBA32Uint), E(RGBA32Sint),
    [0x31] = E(Stencil8), E(Depth16Unorm),
    // DEPTH24：Apple GPU 无 Depth24（Depth24Unorm_Stencil8 iOS 不可用）→ Depth32Float：作渲染目标精度升级、行为等价；
    // 标替代只因上传现成 24 位深度数据时需转浮点（渲染目标不涉及）
    // DEPTH24: Apple GPUs have no Depth24 (Depth24Unorm_Stencil8 is unavailable on iOS) → Depth32Float: as a render target this is a precision upgrade with equivalent behavior;
    // marked substitute only because uploading existing 24-bit depth data requires conversion to float (render targets are not affected)
    [0x33] = S(Depth32Float), E(Depth32Float),
    // DEPTH24_STENCIL8 → Depth32Float_Stencil8（同上：渲染目标精度升级、行为等价；上传数据需转换故标替代）；DEPTH32F_STENCIL8 精确
    // DEPTH24_STENCIL8 → Depth32Float_Stencil8 (as above: precision upgrade as a render target, equivalent behavior; marked substitute because uploaded data needs conversion); DEPTH32F_STENCIL8 is exact
    [0x35] = S(Depth32Float_Stencil8), E(Depth32Float_Stencil8),
    [0x37] = X(RGBA8Unorm_sRGB), E(RGBA8Unorm_sRGB),
    // 16 位打包格式：按名字对应——BGR 系（0x95–0x98）直接对 Metal 同名，RGB 系用同一 Metal 格式并交换 r/b。
    // NVN 与 Metal 的实际位序未用真数据核 → 全部标替代，首次遇到真贴图时核位序再升精确
    // 16-bit packed formats: matched by name - the BGR family (0x95–0x98) maps directly to the same-named Metal format, the RGB family uses the same Metal format with r/b swapped.
    // The actual bit order of NVN vs Metal has not been verified with real data → all marked substitute; verify the bit order when a real texture is first encountered, then upgrade to exact
    [0x39] = S(ABGR4Unorm), SXR(BGR5A1Unorm), SR(BGR5A1Unorm), SR(B5G6R5Unorm),
    [0x3D] = E(RGB10A2Unorm), E(RGB10A2Uint), E(RG11B10Float), E(RGB9E5Float),
    // DXT / RGTC / BPTC = BC1–7；RGB_DXT1 无 alpha
    // DXT / RGTC / BPTC = BC1–7; RGB_DXT1 has no alpha
    [0x41] = X(BC1), E(BC1), E(BC2), E(BC3), X(BC1_sRGB), E(BC1_sRGB), E(BC2_sRGB), E(BC3_sRGB),
    [0x49] = E(BC4_U), E(BC4_S), E(BC5_U), E(BC5_S), E(BC7), E(BC7_sRGB), E(BC6H_S), E(BC6H_U),
    // *_UI2F / *_I2F：整数存储、按浮点读（不归一化转换）。Metal 无此读法 → 替代为同宽整数格式，着色器按整数采样后转 float；
    // 3 分量的同 0x19 组处理（返回 0）
    // *_UI2F / *_I2F: integer storage, read as float (conversion without normalization). Metal has no such read mode → substituted with the same-width integer format; the shader samples as integer and then converts to float;
    // 3-component ones are handled like the 0x19 group (return 0)
    [0x51] = S(R8Uint), S(R8Sint), S(R16Uint), S(R16Sint), S(R32Uint), S(R32Sint),
    [0x57] = S(RG8Uint), S(RG8Sint), S(RG16Uint), S(RG16Sint), S(RG32Uint), S(RG32Sint),
    [0x5D] = N, N, N, N, N, N,
    [0x63] = S(RGBA8Uint), S(RGBA8Sint), S(RGBA16Uint), S(RGBA16Sint), S(RGBA32Uint), S(RGBA32Sint),
    // RGB10A2 有符号 / 有符号整数：Metal 只有 Unorm / Uint → 0；处理：上传时解包到 RGBA16Snorm / RGBA16Sint
    // RGB10A2_UI2F → RGB10A2Uint 着色器转 float（替代）；RGB10A2_I2F → 0（同上解包到 RGBA16Sint）
    // RGB10A2 signed / signed integer: Metal only has Unorm / Uint → 0; handling: unpack to RGBA16Snorm / RGBA16Sint on upload
    // RGB10A2_UI2F → RGB10A2Uint with the shader converting to float (substitute); RGB10A2_I2F → 0 (unpacked to RGBA16Sint as above)
    [0x69] = N, N, S(RGB10A2Uint), N,
    [0x6D] = X(RGBA8Unorm), X(RGBA8Snorm), X(RGBA8Uint), X(RGBA8Sint),
    [0x71] = X(RGBA16Float), X(RGBA16Unorm), X(RGBA16Snorm), X(RGBA16Uint), X(RGBA16Sint),
    [0x76] = X(RGBA32Float), X(RGBA32Uint), X(RGBA32Sint),
    // 0x79–0x94 ASTC 由 astc_off 计算（LDR / sRGB 两组各 14 种）
    // 0x79–0x94 ASTC is computed by astc_off (two groups, LDR / sRGB, 14 each)
    [0x95] = S(B5G6R5Unorm), SX(BGR5A1Unorm), S(BGR5A1Unorm), S(A1BGR5Unorm),
    [0x99] = X(BGRA8Unorm), E(BGRA8Unorm), X(BGRA8Unorm_sRGB), E(BGRA8Unorm_sRGB),
};

// NVN ASTC 序：4x4 5x4 5x5 6x5 6x6 8x5 8x6 8x8 10x5 10x6 10x8 10x10 12x10 12x12；Metal 在 6x6 与 8x5 之间空一号
// NVN ASTC order: 4x4 5x4 5x5 6x5 6x6 8x5 8x6 8x8 10x5 10x6 10x8 10x10 12x10 12x12; Metal skips one number between 6x6 and 8x5
static const uint8_t astc_off[14] = {0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14};

int nvn_fmt_to_mtl(int f, uint32_t *mtl, uint8_t swz[4]) {
    ent e = {0, 0, 0, 0};
    if (f >= 0x79 && f <= 0x94) { int k = (f - 0x79) % 14; e = (ent){(uint16_t)((f < 0x87 ? ASTC_LDR0 : ASTC_SRGB0) + astc_off[k]), 1, 0, 0}; }
    else if (f > 0 && f < (int)(sizeof T / sizeof *T)) e = T[f];
    if (mtl) *mtl = e.kind ? e.mtl : 0;
    if (swz) { swz[0] = e.rb ? NVN_SWZ_B : NVN_SWZ_R; swz[1] = NVN_SWZ_G; swz[2] = e.rb ? NVN_SWZ_R : NVN_SWZ_B; swz[3] = e.noalpha ? NVN_SWZ_ONE : NVN_SWZ_A; }
    return e.kind;
}

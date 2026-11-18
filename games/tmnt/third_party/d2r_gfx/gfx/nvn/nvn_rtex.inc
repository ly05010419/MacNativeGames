// nvn_rtex.inc —— 运行时外推的着色器（由 nvn_mtl.m 在 rp_end / m4_end / res_add / args_dirty_all / blit 等定义之后 #include）
//
// int rt_ex_encode(dst_hdr, dst_ds, src_hdr, src_ds, vp_prev[16], vp_cur[16], unit_bits, ui_bits)
//   在当前 MTL4 命令缓冲 g_cb 上编一个渲染通道（全屏一个三角形），把上一全画帧的不透明 HDR 与深度按本帧相机外推进 dst_hdr / dst_ds：
//   反向映射：目标像素 q → 源像素 p，用源深度做 2 次不动点迭代；M = inv(vp_prev) · vp_cur 在 CPU 上双精度算（1e4 量级平移下单精度逆矩阵会差出 0.01 像素）。
//   非洞：五抽头 Catmull-Rom，按 2×2 核心纹素的最小 / 最大值夹住防振铃，再按位移的小数相位补锐化（相位 0.5 最强、整数相位为 0，强度 k = 0.5）。
//   洞（源 2×2 核心的模板含 unit_bits | ui_bits：旧单位位置 / 界面挖空）：沿 8 个方向每 4 像素往外找第一个非洞、非纯黑的像素（最远 24 步），
//   按 1/距离² 加权平均；深度取其中最近那个。洞和屏边新露出（源越界）的像素由片元导出模板 0x80，之后的区域后处理据此重算。
//   本帧界面挡住的像素（界面模板趟已写 0x2）按模板测试跳过。
// 返回 1 = 编了；0 = 尺寸不齐 / 建不出管线（调用方 rt_copy_prev 先用 rx_setup 预检，建不出就整局退回原画法）。
// 调用前后：本函数先 rp_end() 收掉当前渲染通道与拷贝编码器；编完置 args_dirty_all() / g_pso_dirty / g_dss_dirty，游戏下一个绘制按「读入」重开它的通道。
// 资源：src_hdr / src_ds 由调用方建并 res_add；src_ds 要能开 X32_Stencil8 视图（建时带 MTLTextureUsagePixelFormatView）。
// nvn_rtex.inc - shaders for runtime extrapolation (#included by nvn_mtl.m after the definitions of rp_end / m4_end / res_add / args_dirty_all / blit etc.)
// int rt_ex_encode(dst_hdr, dst_ds, src_hdr, src_ds, vp_prev[16], vp_cur[16], unit_bits, ui_bits)
//   Encodes one render pass (a single fullscreen triangle) on the current MTL4 command buffer g_cb, extrapolating the opaque HDR and depth of the previous fully drawn frame into dst_hdr / dst_ds by this frame's camera:
//   Backward mapping: destination pixel q → source pixel p, with 2 fixed-point iterations using the source depth; M = inv(vp_prev) · vp_cur is computed on the CPU in double precision (with translations on the order of 1e4 a single-precision inverse matrix would be off by 0.01 pixel).
//   Non-hole: five-tap Catmull-Rom, clamped to the min / max of the 2×2 core texels against ringing, then sharpening compensation by the fractional phase of the displacement (strongest at phase 0.5, 0 at integer phase, strength k = 0.5).
//   Hole (the stencil of the source 2×2 core contains unit_bits | ui_bits: old unit position / UI cut-out): along 8 directions search outward every 4 pixels for the first pixel that is neither a hole nor pure black (at most 24 steps),
//   weighted average by 1/distance²; depth takes the nearest of them. Holes and pixels newly exposed at the screen edge (source out of bounds) get stencil 0x80 exported by the fragment, and the later regional post-processing is recomputed accordingly.
//   Pixels covered by this frame's UI (the UI stencil pass already wrote 0x2) are skipped by the stencil test.
// Returns 1 = encoded; 0 = size mismatch / pipeline cannot be built (the caller rt_copy_prev prechecks with rx_setup first, and if it cannot be built the whole run falls back to the original drawing method).
// Before and after the call: this function first calls rp_end() to close the current render pass and copy encoder; after encoding it sets args_dirty_all() / g_pso_dirty / g_dss_dirty, and the game's next draw reopens its pass with 'load'.
// Resources: src_hdr / src_ds are created and res_add'ed by the caller; src_ds must be able to open an X32_Stencil8 view (created with MTLTextureUsagePixelFormatView).
static id<MTLRenderPipelineState> g_rx_pso1; static MTLPixelFormat g_rx_fmt_c, g_rx_fmt_ds;
static id<MTLDepthStencilState> g_rx_dss; static id<MTL4ArgumentTable> g_rx_args;
static id<MTLBuffer> g_rx_const[3]; static int g_rx_ci;
static id<MTLTexture> g_rx_stv; static __unsafe_unretained id<MTLTexture> g_rx_stv_src;
static const char *k_rx_msl =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VO { float4 p [[position]]; };\n"
"vertex VO rx_vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); VO o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
"struct K { float4x4 M; uint hole; float amount; float2 pad; };\n"
"static float4 cr_w(float t) { return float4(((-0.5 * t + 1.0) * t - 0.5) * t, (1.5 * t - 2.5) * t * t + 1.0, ((-1.5 * t + 2.0) * t + 0.5) * t, (0.5 * t - 0.5) * t * t); }\n"
"static uint stv(texture2d<uint> s, int2 p) { uint4 v = s.read(uint2(p)); return v.r | v.g; }\n"   // X32_Stencil8 视图：模板值在哪个分量随实现，r | g 两处都取（另一处为 0） | X32_Stencil8 view: which component holds the stencil value is implementation-dependent, take both r | g (the other one is 0)
"static float zproj(float4x4 M, float2 p, float z, int W, int H) { float4 c = M * float4((p.x + 0.5) / W * 2 - 1, 1 - (p.y + 0.5) / H * 2, z, 1); return c.z / c.w; }\n"
"struct O1 { float4 c [[color(0)]]; float d [[depth(any)]]; uint s [[stencil]]; };\n"
"fragment O1 rx_fs1(VO in [[stage_in]], texture2d<float> hdr [[texture(0)]], depth2d<float> dep [[texture(1)]], texture2d<uint> stc [[texture(2)]], constant K& k [[buffer(0)]]) {\n"
"  constexpr sampler ls(filter::linear, address::clamp_to_edge, coord::pixel);\n"
"  int W = hdr.get_width(), H = hdr.get_height(); int2 hi = int2(W - 1, H - 1);\n"
"  float2 q = floor(in.p.xy); float2 p = q;\n"
"  for (int it = 0; it < 2; it++) {\n"
"    int2 pi = clamp(int2(rint(p)), int2(0), hi); float z = dep.read(uint2(pi));\n"
"    float4 c = k.M * float4((p.x + 0.5) / W * 2 - 1, 1 - (p.y + 0.5) / H * 2, z, 1);\n"
"    float2 qq = float2((c.x / c.w + 1) * 0.5 * W - 0.5, (1 - c.y / c.w) * 0.5 * H - 0.5); p += q - qq; }\n"
"  bool oob = p.x < -0.5 || p.y < -0.5 || p.x > float(hi.x) + 0.5 || p.y > float(hi.y) + 0.5;\n"
"  p = clamp(p, float2(0), float2(hi));\n"
"  int2 b0 = clamp(int2(floor(p)), int2(0), hi - 1);\n"
"  uint s4 = stv(stc, b0) | stv(stc, b0 + int2(1, 0)) | stv(stc, b0 + int2(0, 1)) | stv(stc, b0 + int2(1, 1));\n"
"  int2 cp = clamp(int2(rint(p)), int2(0), hi);\n"
"  O1 o; o.s = ((s4 & k.hole) || oob) ? 0x80u : 0u;\n"
"  if (!(s4 & k.hole)) {\n"
"    float3 t00 = hdr.read(uint2(b0)).rgb, t10 = hdr.read(uint2(b0 + int2(1, 0))).rgb, t01 = hdr.read(uint2(b0 + int2(0, 1))).rgb, t11 = hdr.read(uint2(b0 + int2(1, 1))).rgb;\n"
"    float3 lo = min(min(t00, t10), min(t01, t11)), hc = max(max(t00, t10), max(t01, t11)), avg = (t00 + t10 + t01 + t11) * 0.25;\n"
"    float2 sp = p + 0.5; float2 tp1 = floor(sp - 0.5) + 0.5; float2 f = sp - tp1;\n"   // 五抽头 Catmull-Rom（双线性取样合并中间两抽头、丢四角） | Five-tap Catmull-Rom (bilinear sampling merges the two middle taps, the four corners are dropped)
"    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f), w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
"    float2 w12 = w1 + w2, t12 = tp1 + w2 / w12, t0 = tp1 - 1.0, t3 = tp1 + 2.0;\n"
"    float3 acc = hdr.sample(ls, float2(t12.x, t0.y)).rgb * (w12.x * w0.y) + hdr.sample(ls, float2(t0.x, t12.y)).rgb * (w0.x * w12.y) + hdr.sample(ls, t12).rgb * (w12.x * w12.y)\n"
"               + hdr.sample(ls, float2(t3.x, t12.y)).rgb * (w3.x * w12.y) + hdr.sample(ls, float2(t12.x, t3.y)).rgb * (w12.x * w3.y);\n"
"    float wt = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y; float3 cr = clamp(acc / wt, lo, hc);\n"
"    o.c = float4(max(cr + k.amount * (cr - avg), 0.0), 1); o.d = zproj(k.M, p, dep.read(uint2(cp)), W, H); return o; }\n"   // 按小数相位补锐化并进同一趟（邻域取 2×2 核心的均值） | Sharpening compensation by fractional phase is folded into the same pass (the neighborhood takes the mean of the 2×2 core)
"  const int2 dirs[8] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1), int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1) };\n"
"  float3 sum = 0; float wsum = 0; int bestr = 1000; int2 bests = cp;\n"
"  for (int d = 0; d < 8; d++) for (int r = 1; r <= 24; r++) { int2 s = cp + dirs[d] * (4 * r);\n"
"    if (s.x < 0 || s.y < 0 || s.x > hi.x || s.y > hi.y) break;\n"
"    if (stv(stc, s) & k.hole) continue;\n"
"    float3 v = hdr.read(uint2(s)).rgb; if (max(v.r, max(v.g, v.b)) <= 0) continue;\n"
"    float w = 1.0 / float(r * r); sum += v * w; wsum += w; if (r < bestr) { bestr = r; bests = s; } break; }\n"
"  if (wsum > 0) { o.c = float4(sum / wsum, 1); o.d = zproj(k.M, float2(bests), dep.read(uint2(bests)), W, H); return o; }\n"
"  o.c = float4(hdr.read(uint2(cp)).rgb, 1); o.d = zproj(k.M, p, dep.read(uint2(cp)), W, H); return o;\n"
"}\n";
static int rx_setup(MTLPixelFormat fc, MTLPixelFormat fds) {
    if (getenv("DM_RT_TEST_NOPSO")) return 0;   // 测试：当作这台设备建不出外推管线（验整局退回原画法） | Test: pretend this device cannot build the extrapolation pipeline (verifies the whole run falls back to the original drawing method)
    if (g_rx_pso1 && g_rx_fmt_c == fc && g_rx_fmt_ds == fds) return 1;
    static int failed; if (failed) return 0;
    NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_rx_msl) options:nil error:&e];
    if (!lib) { MLOG("【红】运行时外推：着色器编译失败 %s", e.localizedDescription.UTF8String); failed = 1; return 0; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [lib newFunctionWithName:@"rx_vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"rx_fs1"];
    pd.colorAttachments[0].pixelFormat = fc; pd.depthAttachmentPixelFormat = fds;
    if (fds == MTLPixelFormatDepth32Float_Stencil8) pd.stencilAttachmentPixelFormat = fds;
    g_rx_pso1 = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!g_rx_pso1) { MLOG("【红】运行时外推：管线建不出 %s", e.localizedDescription.UTF8String); g_rx_pso1 = nil; failed = 1; return 0; }
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new]; dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = YES;
    if (fds == MTLPixelFormatDepth32Float_Stencil8) { MTLStencilDescriptor *sd = [MTLStencilDescriptor new]; sd.stencilCompareFunction = MTLCompareFunctionEqual; sd.readMask = 0x2; sd.writeMask = 0x80; sd.depthStencilPassOperation = MTLStencilOperationReplace;   // 洞 / 屏外新进来的像素由片元导出 0x80，后面 LDR 区域据此重算   // 本帧被界面挡住的像素（界面模板趟已写 0x2）不算 | Hole pixels / pixels newly entering from off-screen get 0x80 exported by the fragment, and the later LDR region is recomputed accordingly   // Pixels covered by this frame's UI (the UI stencil pass already wrote 0x2) do not count
        dd.frontFaceStencil = sd; dd.backFaceStencil = sd; }
    g_rx_dss = [g_dev newDepthStencilStateWithDescriptor:dd];
    if (!g_rx_args) { MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 3; ad.maxBufferBindCount = 1;
        g_rx_args = [g_dev newArgumentTableWithDescriptor:ad error:&e];
        for (int i = 0; i < 3; i++) { g_rx_const[i] = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; res_add(g_rx_const[i]); } }
    if (!g_rx_args || !g_rx_dss) { MLOG("【红】运行时外推：参数表 / 深度状态建不出"); g_rx_pso1 = nil; failed = 1; return 0; }
    g_rx_fmt_c = fc; g_rx_fmt_ds = fds; MLOG("运行时外推：着色器建好（颜色格式 %lu，深度格式 %lu）", (unsigned long)fc, (unsigned long)fds);
    return 1;
}
static void rx_inv4(const double *m, double *o) {   // 4×4 求逆（伴随矩阵法，双精度） | 4×4 inverse (adjugate method, double precision)
    double inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    double det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12]; det = det != 0 ? 1.0 / det : 0;
    for (int i = 0; i < 16; i++) o[i] = inv[i] * det;
}
int rt_ex_encode(id<MTLTexture> dst_hdr, id<MTLTexture> dst_ds, id<MTLTexture> src_hdr, id<MTLTexture> src_ds,
                 const float vp_prev[16], const float vp_cur[16], uint32_t unit_bits, uint32_t ui_bits) {
    if (!dst_hdr || !dst_ds || !src_hdr || !src_ds || !vp_prev || !vp_cur || !g_cb) return 0;
    if (src_hdr.width != dst_hdr.width || src_hdr.height != dst_hdr.height || src_ds.width != src_hdr.width || src_ds.height != src_hdr.height
        || dst_ds.width != dst_hdr.width || dst_ds.height != dst_hdr.height) return 0;   // 换了分辨率（读取画面 / 动态比例）：这帧不外推 | Resolution changed (loading screen / dynamic scale): no extrapolation this frame
    if (!rx_setup(dst_hdr.pixelFormat, dst_ds.pixelFormat)) return 0;
    if (g_rx_stv_src != src_ds) {
        if (g_rx_stv) defer_obj_release(g_rx_stv);
        g_rx_stv = [src_ds newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
        if (!g_rx_stv) { static int n; if (!n++) MLOG("【红】运行时外推：开不出模板视图（src_ds 要带 PixelFormatView 用途）"); return 0; }
        res_add(g_rx_stv); g_rx_stv_src = src_ds;
    }
    // 常量：M = inv(vp_prev) · vp_cur（行向量：clip_cur = (ndc_prev, z, 1) · M）；Metal 的 float4x4 按列存，行向量矩阵的第 i 行正好当第 i 列 ⇒ 着色器里写 M * v
    // Constant: M = inv(vp_prev) · vp_cur (row vectors: clip_cur = (ndc_prev, z, 1) · M); Metal's float4x4 is stored by column, and row i of the row-vector matrix serves exactly as column i ⇒ write M * v in the shader
    double a[16], b[16], ia[16], m[16];
    for (int i = 0; i < 16; i++) { a[i] = vp_prev[i]; b[i] = vp_cur[i]; }
    rx_inv4(a, ia);
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { double s = 0; for (int k = 0; k < 4; k++) s += ia[r * 4 + k] * b[k * 4 + c]; m[r * 4 + c] = s; }
    // 小数相位：世界原点（任一点皆可，正交 + 平移全屏一致）在两帧屏幕上的像素位移 d = 目标 − 源；反向映射的源 = 目标 − d
    // Fractional phase: pixel displacement on screen between the two frames of the world origin (any point works; orthographic + translation is uniform across the screen), d = destination − source; the source of the backward mapping = destination − d
    double W = dst_hdr.width, H = dst_hdr.height;
    double c0x = m[12] / m[15], c0y = m[13] / m[15];   // 源 NDC 原点 (0, 0, 0, 1) 投到本帧 | Source NDC origin (0, 0, 0, 1) projected into this frame
    double dx = c0x / 2 * W, dy = -c0y / 2 * H, sx = -dx, sy = -dy;   // 源相对目标的偏移（像素） | Offset of the source relative to the destination (pixels)
    double fx = sx - floor(sx), fy = sy - floor(sy), amount = 0.5 * 2 * (fx * (1 - fx) + fy * (1 - fy));
    float *k = g_rx_const[g_rx_ci].contents;
    for (int i = 0; i < 16; i++) k[i] = (float)m[i];
    ((uint32_t *)k)[16] = unit_bits | ui_bits; k[17] = (float)amount;
    id<MTLBuffer> cbuf = g_rx_const[g_rx_ci]; g_rx_ci = (g_rx_ci + 1) % 3;
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    // 单趟：外推 + 补洞 + 按小数相位补锐化 → dst_hdr；深度写进 dst_ds；本帧界面挡住的像素按模板跳过（模板不写）
    // Single pass: extrapolation + hole filling + sharpening compensation by fractional phase → dst_hdr; depth is written into dst_ds; pixels covered by this frame's UI are skipped by stencil (stencil not written)
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = dst_hdr; rp.colorAttachments[0].loadAction = MTLLoadActionLoad; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.depthAttachment.texture = dst_ds; rp.depthAttachment.loadAction = MTLLoadActionLoad; rp.depthAttachment.storeAction = MTLStoreActionStore;
    if (dst_ds.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) { rp.stencilAttachment.texture = dst_ds; rp.stencilAttachment.loadAction = MTLLoadActionLoad; rp.stencilAttachment.storeAction = MTLStoreActionStore; }
    id<MTL4RenderCommandEncoder> en = [g_cb renderCommandEncoderWithDescriptor:rp]; if (!en) return 0;
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [en setArgumentTable:g_rx_args atStages:MTLRenderStageFragment];
    [g_rx_args setTexture:src_hdr.gpuResourceID atIndex:0]; [g_rx_args setTexture:src_ds.gpuResourceID atIndex:1]; [g_rx_args setTexture:g_rx_stv.gpuResourceID atIndex:2];
    [g_rx_args setAddress:cbuf.gpuAddress atIndex:0];
    [en setViewport:(MTLViewport){0, 0, W, H, 0, 1}]; [en setRenderPipelineState:g_rx_pso1]; [en setDepthStencilState:g_rx_dss]; [en setStencilReferenceValue:0];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; m4_end(en);
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
    return 1;
}

// 正交相机下两帧之间整幅平移的像素量（取整）：目标 = 源 + (dx, dy)
// Whole-image translation in pixels between two frames under an orthographic camera (rounded): destination = source + (dx, dy)
static void rt_shift_px(const float vp_prev[16], const float vp_cur[16], double W, double H, int *dx, int *dy) {
    double a[16], b[16], ia[16], m[16];
    for (int i = 0; i < 16; i++) { a[i] = vp_prev[i]; b[i] = vp_cur[i]; }
    rx_inv4(a, ia);
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { double s = 0; for (int k = 0; k < 4; k++) s += ia[r * 4 + k] * b[k * 4 + c]; m[r * 4 + c] = s; }
    *dx = (int)lround(m[12] / m[15] / 2 * W); *dy = (int)lround(-m[13] / m[15] / 2 * H);
}

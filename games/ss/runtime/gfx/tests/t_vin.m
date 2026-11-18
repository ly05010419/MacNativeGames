// Shadow vertex inputs (dm/gfx/tools/dmsh_vin.py, nvn_mtl.m vin_ct): an integer shader input also declared as a float4 and as the integer of the other sign, the three ORed, with the
// attribute routed to the declaration of its format's class and the zero buffer to the others. Checks that Metal's fetch then gives the 32-bit register values the CPU conversion
// (nvn_mtl.m vconv_elem) made: the converted float32 bits for half / normalized / packed formats (missing components 0, 0, 0, 1.0), the value widened by the format's own sign for
// 8 / 16-bit integers (missing 0, 0, 0, 1), into a uint input and into an int input. The reference below follows vconv_elem's formulas; half and integer formats must match exactly,
// normalized formats within one ULP. Negative control: the same data routed to the input's own declaration with the other class's format is refused by Metal or differs.
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *k_msl =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct main0_in { uint4 m_a [[attribute(0)]]; int4 m_b [[attribute(1)]]; };\n"
    "struct main0_in_v { uint4 m_a [[attribute(0)]]; int4 m_b [[attribute(1)]];\n"
    "    float4 m_a_vf [[attribute(16)]]; float4 m_b_vf [[attribute(17)]]; int4 m_a_vo [[attribute(18)]]; uint4 m_b_vo [[attribute(19)]]; };\n"
    "vertex void vs(main0_in_v dm_in [[stage_in]], uint vid [[vertex_id]], device uint4 *o [[buffer(1)]])\n{\n"
    "    main0_in in = {};\n"
    "    in.m_a = dm_in.m_a | as_type<uint4>(dm_in.m_a_vf) | as_type<uint4>(dm_in.m_a_vo);\n"
    "    in.m_b = dm_in.m_b | as_type<int4>(dm_in.m_b_vf) | as_type<int4>(dm_in.m_b_vo);\n"
    "    o[2 * vid] = in.m_a; o[2 * vid + 1] = as_type<uint4>(in.m_b);\n}\n";

enum { NV = 64, CF = 0, CU = 1, CI = 2 };   // vertices; format classes: float-type, unsigned, signed
typedef struct { const char *name; MTLVertexFormat mf; int cls, n, size, kind; } fmt_t;   // kind: how the reference converts (below)
enum { K_HALF, K_UN8, K_SN8, K_UN16, K_SN16, K_U8, K_I8, K_U16, K_I16, K_1010102, K_11_11_10 };
static const fmt_t F[] = {
    { "Half2", MTLVertexFormatHalf2, CF, 2, 4, K_HALF }, { "Half4", MTLVertexFormatHalf4, CF, 4, 8, K_HALF }, { "UChar4Normalized", MTLVertexFormatUChar4Normalized, CF, 4, 4, K_UN8 },
    { "Char2Normalized", MTLVertexFormatChar2Normalized, CF, 2, 2, K_SN8 }, { "UShort2Normalized", MTLVertexFormatUShort2Normalized, CF, 2, 4, K_UN16 },
    { "Short4Normalized", MTLVertexFormatShort4Normalized, CF, 4, 8, K_SN16 }, { "UInt1010102Normalized", MTLVertexFormatUInt1010102Normalized, CF, 4, 4, K_1010102 },
    { "FloatRG11B10", MTLVertexFormatFloatRG11B10, CF, 3, 4, K_11_11_10 },
    { "UChar4", MTLVertexFormatUChar4, CU, 4, 4, K_U8 }, { "UShort2", MTLVertexFormatUShort2, CU, 2, 4, K_U16 },
    { "Char4", MTLVertexFormatChar4, CI, 4, 4, K_I8 }, { "Short2", MTLVertexFormatShort2, CI, 2, 4, K_I16 }, { "Short4", MTLVertexFormatShort4, CI, 4, 8, K_I16 },
};

static float uf(uint32_t v, int mbits) { uint32_t e = v >> mbits & 31, m = v & ((1u << mbits) - 1); if (!e) return ldexpf((float)m, -14 - mbits); if (e == 31) return m ? NAN : INFINITY; return ldexpf(1.0f + (float)m / (float)(1u << mbits), (int)e - 15); }
static void ref(const fmt_t *f, const uint8_t *p, uint32_t o[4]) {   // as vconv_elem: the register's four 32-bit values
    if (f->cls != CF) { o[0] = o[1] = o[2] = 0; o[3] = 1;
        for (int i = 0; i < f->n; i++) switch (f->kind) {
            case K_U8: o[i] = p[i]; break; case K_I8: o[i] = (uint32_t)(int32_t)(int8_t)p[i]; break;
            case K_U16: { uint16_t h; memcpy(&h, p + 2 * i, 2); o[i] = h; break; } case K_I16: { uint16_t h; memcpy(&h, p + 2 * i, 2); o[i] = (uint32_t)(int32_t)(int16_t)h; break; } }
        return; }
    float v[4] = { 0, 0, 0, 1 };
    for (int i = 0; i < f->n; i++) switch (f->kind) {
        case K_HALF: { _Float16 h; memcpy(&h, p + 2 * i, 2); v[i] = (float)h; break; }
        case K_UN8: v[i] = p[i] / 255.0f; break; case K_SN8: v[i] = fmaxf((int8_t)p[i] / 127.0f, -1.0f); break;
        case K_UN16: { uint16_t u; memcpy(&u, p + 2 * i, 2); v[i] = u / 65535.0f; break; } case K_SN16: { int16_t s; memcpy(&s, p + 2 * i, 2); v[i] = fmaxf(s / 32767.0f, -1.0f); break; }
        case K_1010102: { uint32_t w; memcpy(&w, p, 4); uint32_t bits = i == 3 ? 2 : 10; v[i] = (float)(w >> (10 * i) & ((1u << bits) - 1)) / (float)((1u << bits) - 1); break; }
        case K_11_11_10: { uint32_t w; memcpy(&w, p, 4); v[i] = uf(i == 0 ? w & 0x7ff : i == 1 ? w >> 11 & 0x7ff : w >> 22, i == 2 ? 5 : 6); break; }
    }
    memcpy(o, v, 16);
}
static uint32_t ulps(uint32_t a, uint32_t b) { int32_t x = (int32_t)a, y = (int32_t)b; if ((x < 0) != (y < 0)) return a == b ? 0 : UINT32_MAX; return (uint32_t)(x > y ? x - y : y - x); }

static int g_n, g_fail;
static void check(const char *what, int ok, const char *detail) { g_n++; if (!ok) g_fail++; printf("%-62s %s %s\n", what, ok ? "✓" : "✗", ok ? "" : detail); }

int main(void) {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(k_msl) options:nil error:&err];
    if (!lib) { printf("MSL compile failed: %s\n", err.localizedDescription.UTF8String); return 1; }
    id<MTLFunction> fn = [lib newFunctionWithName:@"vs"]; id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLBuffer> zero = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared], out = [dev newBufferWithLength:NV * 32 options:MTLResourceStorageModeShared];
    memset(zero.contents, 0, 64);
    const int nf = (int)(sizeof F / sizeof *F);
    // input a (uint) gets every format, input b (int) the next one in the list: each pipeline mixes two classes, as Age of Calamity's soldiers do
    for (int ia = 0; ia < nf; ia++) {
        const fmt_t *fa = &F[ia], *fb = &F[(ia + 1) % nf];
        uint8_t da[NV * 8], db[NV * 8];
        for (int i = 0; i < NV * 8; i++) { da[i] = (uint8_t)(i * 37 + 11 ^ i >> 3); db[i] = (uint8_t)(i * 91 + 5 ^ 0xA5); }
        for (int i = 0; i < NV * 8; i += 2) { if (fa->kind == K_HALF && (da[i + 1] & 0x7c) == 0x7c) da[i + 1] &= 0xbb; if (fb->kind == K_HALF && (db[i + 1] & 0x7c) == 0x7c) db[i + 1] &= 0xbb; }   // no half NaN / inf
        id<MTLBuffer> ba = [dev newBufferWithBytes:da length:sizeof da options:MTLResourceStorageModeShared], bb = [dev newBufferWithBytes:db length:sizeof db options:MTLResourceStorageModeShared];
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        // the routing vin_ct makes: own class → the input's declaration, else the shadow of the format's class; every other declaration reads the zero buffer
        const int idx_a[3] = { 16, 0, 18 }, idx_b[3] = { 17, 19, 1 };   // by class CF / CU / CI
        const MTLVertexFormat zf[3] = { MTLVertexFormatFloat4, MTLVertexFormatUInt4, MTLVertexFormatInt4 };
        for (int c = 0; c < 3; c++) {
            int ia_ = idx_a[c], ib_ = idx_b[c];
            if (c == fa->cls) { vd.attributes[ia_].format = fa->mf; vd.attributes[ia_].bufferIndex = 2; } else { vd.attributes[ia_].format = zf[c]; vd.attributes[ia_].bufferIndex = 30; }
            if (c == fb->cls) { vd.attributes[ib_].format = fb->mf; vd.attributes[ib_].bufferIndex = 3; } else { vd.attributes[ib_].format = zf[c]; vd.attributes[ib_].bufferIndex = 30; }
            vd.attributes[ia_].offset = vd.attributes[ib_].offset = 0;
        }
        vd.layouts[2].stride = 8; vd.layouts[3].stride = 8;
        vd.layouts[30].stride = 16; vd.layouts[30].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[30].stepRate = 0;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = fn; pd.rasterizationEnabled = NO; pd.vertexDescriptor = vd;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
        char name[96]; snprintf(name, sizeof name, "%s into uint, %s into int", fa->name, fb->name);
        if (!ps) { check(name, 0, err.localizedDescription.UTF8String); continue; }
        memset(out.contents, 0xEE, NV * 32);
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor]; rp.renderTargetWidth = 1; rp.renderTargetHeight = 1; rp.defaultRasterSampleCount = 1;
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
        [en setRenderPipelineState:ps]; [en setVertexBuffer:out offset:0 atIndex:1]; [en setVertexBuffer:ba offset:0 atIndex:2]; [en setVertexBuffer:bb offset:0 atIndex:3]; [en setVertexBuffer:zero offset:0 atIndex:30];
        [en drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:NV]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
        const uint32_t *o = out.contents; uint32_t worst = 0; char detail[160] = "";
        for (int v = 0; v < NV; v++) for (int s = 0; s < 2; s++) {
            const fmt_t *f = s ? fb : fa; uint32_t e[4]; ref(f, (s ? db : da) + 8 * v, e);
            for (int c = 0; c < 4; c++) { uint32_t d = ulps(o[8 * v + 4 * s + c], e[c]);
                if (d > worst) { worst = d; snprintf(detail, sizeof detail, "vertex %d %s component %d: got %#x, expected %#x (%u ULP)", v, f->name, c, o[8 * v + 4 * s + c], e[c], d); } }
        }
        int exact = (fa->kind == K_HALF || fa->cls != CF) && (fb->kind == K_HALF || fb->cls != CF);
        char what[128]; snprintf(what, sizeof what, "%s: %s", name, exact ? "bit-exact" : "within 1 ULP");
        check(what, exact ? worst == 0 : worst <= 1, detail);
    }
    // negative control: Half2 straight into the uint input (no shadow) is refused by Metal, which is why the shadows exist
    {
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatHalf2; vd.attributes[0].bufferIndex = 2;
        vd.attributes[1].format = MTLVertexFormatShort2; vd.attributes[1].bufferIndex = 3;
        for (int i = 16; i < 20; i++) { vd.attributes[i].format = i == 18 ? MTLVertexFormatInt4 : i == 19 ? MTLVertexFormatUInt4 : MTLVertexFormatFloat4; vd.attributes[i].bufferIndex = 30; }
        vd.layouts[2].stride = 8; vd.layouts[3].stride = 8; vd.layouts[30].stride = 16; vd.layouts[30].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[30].stepRate = 0;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = fn; pd.rasterizationEnabled = NO; pd.vertexDescriptor = vd;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
        check("negative control: Half2 into a uint input is refused by Metal", ps == nil, "Metal built the pipeline");
    }
    printf("== %d checks, %d failed ==\n", g_n, g_fail);
    return g_fail != 0;
}

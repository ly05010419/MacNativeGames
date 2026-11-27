/* zeldaVR, Ocarina of Time: the room around the screens (vr.md row 4; why and how in zeldaVR/runtime/oot_vr_passthrough.h). Included
   by totk_vr.m (GAME_OOT, after oot_vr_window_draw.h); the VR thread only.

   Option (the VR page): passthrough 0 dark around the screens (what players have now; HookshotVR's vr_window_room default is dark too),
   1 the room, through the Quest's chroma key. Only while a screen is shown (the curved screen, the window, the big screen: a picture
   with no head pose of its own); the head view (normal first person) never shows the room.

   A headset frame with the key on is drawn as always, but into g_pt_scratch and over a clear alpha of 0, so each pixel's alpha says
   whether anything was drawn there. Then two passes write the headset image: pt_cov marks each OOT_PT_BORDER_PX square block that has
   anything drawn in it; pt_key writes the key colour where neither its block nor the eight around it has anything (so everything
   drawn keeps a dark border of OOT_PT_BORDER_PX to 2 * OOT_PT_BORDER_PX - 1 pixels: the clear colour), and elsewhere the drawn colour
   pushed out of the key's reach (oot_pt_guard). For OOT_PT_HOLD_NS after the last keyed frame (the head view right after a screen)
   the frame goes through pt_key without the key fill, only the push, as the Quest may still key. */
#include "../runtime/oot_vr_passthrough.h"

int vr_oot_passthrough(void) { return vr_option("passthrough", 0) == 1; }
void vr_oot_set_passthrough(int v) {
    vr_option_set("passthrough", v == 1);
    VLOG("[vr-room] around the screens: %s", v == 1 ? "your room (the Quest's chroma key)" : "dark");
}

static oot_pt_state g_pt; static int g_pt_ready;
static id<MTLTexture> g_pt_scratch, g_pt_cov; static id<MTLRenderPipelineState> g_pt_cov_ps, g_pt_key_ps; static int g_pt_failed;
typedef struct { float key[4], lin_key[4]; float reach; uint32_t fill, pad[2]; } oot_pt_u;
#define OOT_PT_STR2(x) #x
#define OOT_PT_STR(x) OOT_PT_STR2(x)
static const char *k_pt_msl =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "#define B " OOT_PT_STR(OOT_PT_BORDER_PX) "u\n"
    "struct PO { float4 pos [[position]]; };\n"
    "struct K { float4 key, lin_key; float reach; uint fill, pad0, pad1; };\n"
    "vertex PO pt_vs(uint i [[vertex_id]]) {\n"   /* one triangle over the whole target */
    "  float2 p = float2(float((i << 1) & 2u), float(i & 2u)); PO o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
    "fragment float pt_cov(PO i [[stage_in]], texture2d<float, access::read> t [[texture(0)]]) {\n"
    "  const uint2 b = uint2(i.pos.xy) * B, e = uint2(t.get_width() - 1, t.get_height() - 1); float a = 0.0;\n"
    "  for (uint y = 0; y < B; y++) for (uint x = 0; x < B; x++) a = max(a, t.read(min(b + uint2(x, y), e)).a);\n"
    "  return a > 0.0 ? 1.0 : 0.0; }\n"
    /* oot_vr_passthrough.h oot_pt_guard, the same choice of channel */
    "float3 pt_guard(float3 s, constant K &k) {\n"
    "  float3 l = select(pow((s + 0.055) * 0.94786729857, float3(2.4)), s * 0.0773993808, s < 0.04045);\n"
    "  if (!all(abs(l - k.lin_key.rgb) < k.reach)) return s;\n"
    "  float3 to = select(k.lin_key.rgb + k.reach, k.lin_key.rgb - k.reach, k.lin_key.rgb >= 0.5), need = abs(to - l);\n"
    "  if (need.x <= need.y && need.x <= need.z) l.x = to.x; else if (need.y <= need.z) l.y = to.y; else l.z = to.z;\n"
    "  l = clamp(l, 0.0, 1.0);\n"
    "  return select(1.055 * pow(l, float3(1.0 / 2.4)) - 0.055, 12.92 * l, l <= 0.0031308); }\n"
    "fragment float4 pt_key(PO i [[stage_in]], texture2d<float, access::read> t [[texture(0)]],\n"
    "                       texture2d<float, access::read> c [[texture(1)]], constant K &k [[buffer(0)]]) {\n"
    "  const uint2 p = uint2(i.pos.xy);\n"
    "  if (k.fill != 0u) {\n"
    "    const int2 b = int2(p / B), e = int2(c.get_width() - 1, c.get_height() - 1); float n = 0.0;\n"
    "    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) n = max(n, c.read(uint2(clamp(b + int2(x, y), int2(0), e))).r);\n"
    "    if (n <= 0.0) return float4(k.key.rgb, 1.0); }\n"
    "  return float4(pt_guard(t.read(p).rgb, k), 1.0); }\n";

static int oot_pt_pipelines(void) {
    if (g_pt_cov_ps && g_pt_key_ps) return 1;
    if (g_pt_failed) return 0;
    NSError *e = nil; id<MTLLibrary> lib = [g.dev newLibraryWithSource:@(k_pt_msl) options:[MTLCompileOptions new] error:&e];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"pt_vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"pt_cov"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
    g_pt_cov_ps = lib ? [g.dev newRenderPipelineStateWithDescriptor:pd error:&e] : nil;
    pd.fragmentFunction = [lib newFunctionWithName:@"pt_key"]; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_pt_key_ps = lib ? [g.dev newRenderPipelineStateWithDescriptor:pd error:&e] : nil;
    if (!g_pt_cov_ps || !g_pt_key_ps) {
        g_pt_failed = 1;
        VLOG("[vr-room] 【红】no key pipeline (%s): the room stays hidden", e ? e.localizedDescription.UTF8String : "?");
        return 0;
    }
    return 1;
}
static int oot_pt_textures(id<MTLTexture> out) {
    const NSUInteger w = out.width, h = out.height, bw = (w + OOT_PT_BORDER_PX - 1) / OOT_PT_BORDER_PX, bh = (h + OOT_PT_BORDER_PX - 1) / OOT_PT_BORDER_PX;
    if (g_pt_scratch && g_pt_scratch.width == w && g_pt_scratch.height == h && g_pt_cov) return 1;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
    g_pt_scratch = [g.dev newTextureWithDescriptor:d];
    MTLTextureDescriptor *c = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:bw height:bh mipmapped:NO];
    c.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; c.storageMode = MTLStorageModePrivate;
    g_pt_cov = [g.dev newTextureWithDescriptor:c];
    if (!g_pt_scratch || !g_pt_cov) {
        g_pt_scratch = g_pt_cov = nil; g_pt_failed = 1;
        VLOG("[vr-room] 【红】no %lux%lu key images: the room stays hidden", (unsigned long)w, (unsigned long)h);
        return 0;
    }
    return 1;
}

/* Before the eyes are drawn: where they go (out, or g_pt_scratch), the clear colour, and *mode (0 as always, 1 the key around the
   screens, 2 the push only) for oot_pt_finish. Tells the streamer when the key goes on or off. */
static id<MTLTexture> oot_pt_begin(id<MTLTexture> out, id<MTLTexture> __strong *pics, const uint32_t useds[2], MTLClearColor *clear, int *mode) {
    *mode = 0;
    if (!g_pt_ready) { oot_pt_init(&g_pt); g_pt_ready = 1; }
    const int option = vr_oot_passthrough(), offered = vr_link_passthrough_offered();
    if (option && !offered) { static int said; if (!said) { said = 1;
        VLOG("[vr-room] this streamer does not offer passthrough (built before 2026-11-27): run zeldaVR/install_vr.sh again; the room stays hidden"); } }
    const int ok = option && offered && oot_pt_pipelines() && oot_pt_textures(out);
    const int screen = !pics[0] && !pics[1] ? -1 : useds[0] || useds[1] ? 0 : 1;
    const oot_pt_step_out s = oot_pt_step(&g_pt, option, offered && (ok || g_pt.sent == 1), screen, now_ns());
    if (s.send) {
        uint8_t p[8]; oot_pt_payload(s.send_on, p);
        const int r = vr_link_send_passthrough(p);
        static int said = -1; if (said != s.send_on || r) { said = s.send_on;
            VLOG("[vr-room] the Quest's chroma key %s (key %d %d %d, threshold %d, feathering %d%%)%s", s.send_on ? "on" : "off",
                 OOT_PT_KEY_R, OOT_PT_KEY_G, OOT_PT_KEY_B, OOT_PT_THRESHOLD, OOT_PT_FEATHER_PCT, r ? ": not sent, the link is down" : ""); }
    }
    if (!(s.fill_key || s.guard) || !g_pt_scratch || g_pt_scratch.width != out.width || g_pt_scratch.height != out.height || !oot_pt_pipelines()) return out;
    *mode = s.fill_key ? 1 : 2;
    if (s.fill_key) clear->alpha = 0;   /* alpha: anything drawn there */
    return g_pt_scratch;
}
/* After the eyes: the headset image from g_pt_scratch (mode 1: the key around everything drawn; 2: the push only). */
static void oot_pt_finish(id<MTLCommandBuffer> cb, id<MTLTexture> drawn, id<MTLTexture> out, int mode) {
    if (!mode || drawn == out) return;
    oot_pt_u u = { { OOT_PT_KEY_R / 255.0f, OOT_PT_KEY_G / 255.0f, OOT_PT_KEY_B / 255.0f, 1 }, { 0, 0, 0, 0 },
                   OOT_PT_REACH * OOT_PT_THRESHOLD / 255.0f, mode == 1, { 0, 0 } };
    for (int c = 0; c < 3; c++) u.lin_key[c] = oot_pt_lin(u.key[c]);
    if (mode == 1) {
        MTLRenderPassDescriptor *rc = [MTLRenderPassDescriptor renderPassDescriptor];
        rc.colorAttachments[0].texture = g_pt_cov; rc.colorAttachments[0].loadAction = MTLLoadActionDontCare; rc.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rc];
        [e setRenderPipelineState:g_pt_cov_ps]; [e setFragmentTexture:drawn atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
    }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = out; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:g_pt_key_ps]; [e setFragmentTexture:drawn atIndex:0]; [e setFragmentTexture:g_pt_cov atIndex:1];
    [e setFragmentBytes:&u length:sizeof u atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

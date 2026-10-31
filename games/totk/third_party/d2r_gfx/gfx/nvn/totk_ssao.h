// TOTK_SSAO=1 (Mac only, off by default): a tunable version of the screen-space ambient occlusion both games compute in
// their shadow pre-pass. Game-specific and never needed to run the game; included by nvn_mtl.m after totk_msl_library.
// Its hooks there: the NvShader field ssao_slot, dm_gfx_set_flag (ssao_flag), shader_load (ssao_patch, ssao_put),
// variant_library (SSAO_MARK: patched sources stay out of the recorded variants) and pso_archive_key (patched pipelines
// stay out of the pipeline archive).
// Why the shader and not the game's settings: the AO the player sees is computed inside the shadow pre-pass fragment
// shader (TOTK 1.4.3 9a5e3335514e4cbb / abbbf48f9b5e6598 and TotK 1.2.1 dd93068627010533 / 67acb7c322ece44d at render/4,
// BotW 1.9.0 0eeb23f2b287a638 at render/2) and written to the shadow mask's .z. Pair count, radius and strength are
// literals or the shader's own c1 constants, so the game's ssao_parameter data and its agl::sdw::SSAO fields do not reach
// the screen.

/* On the Mac those shaders are rebuilt when they load (shader_load) with a tunable AO, always (user 2026-10-30: the
   settings panel's level acts at once, also from off), unless TOTK_SSAO=0 keeps the shipped ones. Its parameters sit in a free
   float4 of the shader's own c1 (TOTK c1[48], BotW c1[3]): x strength, y radius scale, z extra pairs; all 0 = the stock
   result, bit for bit. TOTK_SSAO_STRENGTH / TOTK_SSAO_RADIUS (percent) / TOTK_SSAO_PAIRS
   set the start values, "flag ssao_strength_pct | ssao_radius_pct | ssao_pairs <n>" change them in play (the draw reads
   c1 from that memory, so no pipeline is rebuilt: an A/B on one frozen view). */
/* The settings panel's choice (Mac, runtime/settings_panel.h, ssao.txt): strength and radius in percent, 0 = off (the
   parameters all 0: the stock AO, bit for bit). Given before the guest starts (dm_gfx_ssao_start): the start values when
   TOTK_SSAO_STRENGTH / TOTK_SSAO_RADIUS are unset. */
static _Atomic int g_ssao_saved;
static int ssao_on(void) {
#if TARGET_OS_IPHONE
    return 0;   // iOS loads prebuilt libraries: no source to rebuild
#else
    static int on; static dispatch_once_t once;   // shader_load runs on the warm-up threads at once
    /* Not for a device's libraries (the iPhone / iPad pipeline-archive tool on the Mac): the device draws the stock AO. */
    dispatch_once(&once, ^{ const char *e = getenv("TOTK_SSAO"); on = e ? strcmp(e, "0") != 0 : !shader_device_libs();
        MLOG("[ssao] %s", on ? "the AO shaders are rebuilt with tunable parameters (the settings panel, flag ssao_strength_pct / ssao_radius_pct / ssao_pairs; TOTK_SSAO=0 keeps the shipped ones)"
                             : "the shipped AO shaders (TOTK_SSAO=0 or a device's libraries)"); });
    return on;
#endif
}
static _Atomic int g_ssao_v[3] = {-1, -1, -1};   // strength %, radius %, extra pairs; -1 = environment not read yet
static const char *const k_ssao_env[3] = {"TOTK_SSAO_STRENGTH", "TOTK_SSAO_RADIUS", "TOTK_SSAO_PAIRS"};
static const int k_ssao_max[3] = {800, 400, 8};
static int ssao_param(int i) {   // the environment value goes in only while the slot is unset (a flag is never overwritten)
    int v = atomic_load(&g_ssao_v[i]);
    if (v < 0) { const char *e = getenv(k_ssao_env[i]); v = e ? atoi(e) : i < 2 ? atomic_load(&g_ssao_saved) : 0; v = v < 0 ? 0 : v > k_ssao_max[i] ? k_ssao_max[i] : v;
                 int was = -1; if (!atomic_compare_exchange_strong(&g_ssao_v[i], &was, v)) v = was; }
    return v;
}
static void ssao_values(float p[4]) { p[0] = ssao_param(0) / 100.0f; p[1] = ssao_param(1) / 100.0f; p[2] = (float)ssao_param(2); p[3] = 0.0f; }
#define SSAO_MARK "// totk-ssao\n"   // first line of a patched source and of every variant made from it (variant_library)

/* TOTK_SSAO: the patched shaders (weak: a library the prediction cache drops goes away). Own lock: g_shcache is the submit
   thread's unlocked dictionary and g_rmu guards the texture / sampler registry. c1 is shared-storage memory the draws read
   directly, so a write reaches the next draws; a frame already in flight may see it part-way (harmless for tuning). */
static NSHashTable<NvShader *> *g_ssao_live; static os_unfair_lock g_ssao_lock = OS_UNFAIR_LOCK_INIT;
static void ssao_slot_write(NvShader *o, const float p[4]) { memcpy((uint8_t *)o->c1buf.contents + o->c1off + 16u * (unsigned)o->ssao_slot, p, 16); }
static void ssao_put(NvShader *o) {   // after c1_store, before any draw can read the slot
    os_unfair_lock_lock(&g_ssao_lock);   // values read under the lock: a flag changing them now writes this shader too
    float p[4]; ssao_values(p);
    if (!g_ssao_live) g_ssao_live = [NSHashTable weakObjectsHashTable];
    ssao_slot_write(o, p); [g_ssao_live addObject:o];
    os_unfair_lock_unlock(&g_ssao_lock);
    MLOG("[ssao] %s: c1 %s, parameters at +%zu", o->key, o->c1buf.length == C1_DECL_SIZE ? "of its own" : "in a shared bank", o->c1off + 16u * (size_t)o->ssao_slot);
}
static int ssao_write_all(void) {
    int n = 0;
    os_unfair_lock_lock(&g_ssao_lock);
    float p[4]; ssao_values(p);
    for (NvShader *o in g_ssao_live.allObjects) { ssao_slot_write(o, p); n++; }
    os_unfair_lock_unlock(&g_ssao_lock);
    return n;
}

/* "flag ssao_strength_pct | ssao_radius_pct | ssao_pairs <n>" (dm_gfx_set_flag): 1 when the name is one of them. */
static int ssao_flag(const char *name, int v) {
    if (strcmp(name, "ssao_strength_pct") && strcmp(name, "ssao_radius_pct") && strcmp(name, "ssao_pairs")) return 0;
    int i = name[5] == 's' ? 0 : name[5] == 'r' ? 1 : 2;
    if (v < 0 || v > k_ssao_max[i]) { MLOG("[flag] %s requires 0..%d (0 = stock)", name, k_ssao_max[i]); return 1; }
    for (int j = 0; j < 3; j++) (void)ssao_param(j);   // the environment's values first: they never overwrite this one
    atomic_store(&g_ssao_v[i], v);
    if (!ssao_on()) { MLOG("【红】[flag] %s = %d kept, but the game started without TOTK_SSAO=1: no shader carries the parameters", name, v); return 1; }
    float p[4]; ssao_values(p); int n = ssao_write_all();
    MLOG("[flag] %s = %d: AO strength %.2f, radius %.2f, extra pairs %.0f in %d shader(s) (0 = stock; the shaders cap strength at 8, radius to 0.25 .. 4, BotW extra sets at 3)", name, v, p[0], p[1], p[2], n); return 1;
}

/* The settings panel's row (Mac): dm_gfx_ssao_start before the guest starts, with the saved strength / radius percent
   (0 = off); dm_gfx_ssao_locked when the environment decides; dm_gfx_ssao_live whether this run's shaders carry the
   parameters (not with TOTK_SSAO=0); dm_gfx_ssao_set changes them in play, 0 back to the stock AO bit for bit. */
void dm_gfx_ssao_start(int pct) { atomic_store(&g_ssao_saved, pct > 0 ? pct : 0); }
int dm_gfx_ssao_locked(void) { return getenv("TOTK_SSAO") || getenv("TOTK_SSAO_STRENGTH") || getenv("TOTK_SSAO_RADIUS"); }
int dm_gfx_ssao_live(void) { return ssao_on(); }
void dm_gfx_ssao_set(int pct) {
    if (!ssao_on()) return;
    ssao_flag("ssao_strength_pct", pct); ssao_flag("ssao_radius_pct", pct);
}

/* TOTK_SSAO source rewrite. The helper goes in front of the fragment entry and the one line that stores the AO value
   (into the shadow-mask .z) calls it instead; every other line, including the screen-space shadow march after it that
   reuses the AO temporaries, is untouched. The helper returns the stock value itself while its parameters are all 0. The
   anchors are the translation's temporaries (chains v6 BotW / v7 TOTK, checked on all seven seed and cache variants; chain
   v8 on 2026-10-29: TOTK's four variants patched in play, BotW's two seed sources patched and compiled offline): another
   translation does not match, and the shader stays stock with a red line. */
static const char k_ssao_helper_totk[] =
    "// ---- TOTK_SSAO: tunable variant of the AO computed in the shadow pre-pass (inserted by the AO patch) ----\n"
    "// Stock: Alchemy-style AO, 8 taps along one rotated screen axis at +/-0.005, +/-0.015, +/-0.025, +/-0.035 (c1[34].y..c1[36].x),\n"
    "// per tap clamp((n.v - 0.03) / (v.v + 1e-4)), occlusion = clamp(sum * 2/N) with N = 8 (the stock \"* 0.25\"), AO = 1 - occlusion.\n"
    "// p = this shader's own c1[48]: x strength (multiplier on the occlusion term, <= 0 -> 1, at most 8), y radius scale\n"
    "// (<= 0 -> 1, else 0.25 .. 4),\n"
    "// z extra +/- tap pairs on the perpendicular axis (0..8, rounded), w reserved. p == 0 returns the stock value untouched.\n"
    "static inline float totk_ssao_tap(float2 uv, float2 dir, float d, float2 fr, float3 pc, float3 nrm,\n"
    "                                  float zs, float zb, texture2d<float> tex, sampler smp)\n"
    "{\n"
    "    // Same operations, in the same order, as one stock tap (view position from linear depth, v = tap - centre).\n"
    "    float2 t = float2(fma(dir.x, d, uv.x), fma(dir.y, d, uv.y));\n"
    "    float z = fma(tex.sample(smp, select(t, float2(0.0), isnan(t))).x, zs, zb);\n"
    "    float vx = fma(z, -((t.x + (-0.5)) * fr.x), -pc.x);\n"
    "    float vy = fma(z, -((t.y + (-0.5)) * fr.y), -pc.y);\n"
    "    float vz = pc.z + (-z);\n"
    "    float nv = fma(nrm.z, vz, fma(nrm.y, vy, nrm.x * vx));\n"
    "    float vv = fma(vz, vz, fma(vy, vy, vx * vx));\n"
    "    return fast::clamp((nv + (-0.02999999932944774627685546875)) * (1.0 / spvFAdd(vv, 9.9999997473787516355514526367188e-05)), 0.0, 1.0);\n"
    "}\n"
    "\n"
    "static inline float totk_ssao_ao(float stock_ao, float stock_sum, float4 p, float2 uv, float2 dir, float2 fr, float3 pc,\n"
    "                                 float3 nrm, float zs, float zb, constant float4* c1, texture2d<float> tex, sampler smp)\n"
    "{\n"
    "    p = select(p, float4(0.0), isnan(p));                 // a NaN parameter counts as 0\n"
    "    if (all(p == float4(0.0)))\n"
    "        return stock_ao;                                   // all zero: the stock value, bit for bit\n"
    "    const float s = p.x > 0.0 ? min(p.x, 8.0) : 1.0;\n"
    "    const float r = p.y > 0.0 ? clamp(p.y, 0.25, 4.0) : 1.0;\n"
    "    const int n = p.z >= 0.5 ? int(min(p.z, 8.0) + 0.5) : 0;\n"
    "    float sum = stock_sum;                                 // the stock 8-tap sum (before \"* 0.25\")\n"
    "    if (r != 1.0)\n"
    "    {\n"
    "        // Re-sample the stock axis at scaled distances; summed in the stock association order.\n"
    "        float a1 = totk_ssao_tap(uv, dir, c1[34].z * r, fr, pc, nrm, zs, zb, tex, smp);\n"
    "        float a2 = totk_ssao_tap(uv, dir, c1[34].y * r, fr, pc, nrm, zs, zb, tex, smp);\n"
    "        float a3 = totk_ssao_tap(uv, dir, c1[35].x * r, fr, pc, nrm, zs, zb, tex, smp);\n"
    "        float a4 = totk_ssao_tap(uv, dir, c1[34].w * r, fr, pc, nrm, zs, zb, tex, smp);\n"
    "        float a5 = totk_ssao_tap(uv, dir, c1[35].z * r, fr, pc, nrm, zs, zb, tex, smp);\n"
    "        float a6 = totk_ssao_tap(uv, dir, c1[35].y * r, fr, pc, nrm, zs, zb, tex, smp);\n"
    "        float a7 = totk_ssao_tap(uv, dir, c1[35].w * r, fr, pc, nrm, zs, zb, tex, smp);\n"
    "        float a8 = totk_ssao_tap(uv, dir, c1[36].x * r, fr, pc, nrm, zs, zb, tex, smp);\n"
    "        sum = (a8 + a7) + (((a1 + a2) + (a3 + a4)) + (a5 + a6));\n"
    "    }\n"
    "    if (n > 0)\n"
    "    {\n"
    "        // Extra pairs on the perpendicular axis, evenly spaced over the full radius R = c1[36].x + c1[34].z (0.04) at\n"
    "        // R (k - 0.5) / n: n = 4 gives the stock distances +/-0.005 .. 0.035, so more pairs smooth without lightening.\n"
    "        const float2 perp = float2(-dir.y, dir.x);\n"
    "        const float R = (c1[36].x + c1[34].z) * r;\n"
    "        float e = 0.0;\n"
    "        for (int k = 1; k <= n; k++)\n"
    "        {\n"
    "            const float d = R * ((float(k) - 0.5) / float(n));\n"
    "            e += totk_ssao_tap(uv, perp, d, fr, pc, nrm, zs, zb, tex, smp) + totk_ssao_tap(uv, perp, -d, fr, pc, nrm, zs, zb, tex, smp);\n"
    "        }\n"
    "        sum += e;\n"
    "    }\n"
    "    // Stock normalisation 2/N (N = 8 -> 0.25), then strength, then radius (a tap's term falls about as 1/distance, so\n"
    "    // without * r a wider radius would read lighter), then the stock clamp and 1 - x.\n"
    "    const float occ = fast::clamp(sum * (2.0 / float(8 + 2 * n)) * s * r, 0.0, 1.0);\n"
    "    return (-occ) + 1.0;\n"
    "}\n"
    "// ---- end TOTK_SSAO ----\n";
static const char k_ssao_helper_botw[] =
    "// ---- TOTK_SSAO (BotW AO in shadow pre-pass 0eeb23f2b287a638, written to .z): live parameters in this shader's c1[3] ----\n"
    "// p = c1[3]: x strength (multiplier on the occlusion term, 0 -> 1, at most 8), y radius scale (tap distances, 0 -> 1,\n"
    "// else 0.25 .. 4), z extra rotated copies of the stock inner/outer pair set (1..3; 0 -> none), each thresholded and\n"
    "// clamped on its own and averaged with the stock set afterwards (so the mean darkness stays and the noise drops), w reserved.\n"
    "// p == 0 returns the stock value untouched. The pair maths repeats the stock code operation for operation, so any p with\n"
    "// x in {0,1}, y in {0,1}, z == 0 also gives the stock value bit for bit (same 4 tap depths, same order of operations).\n"
    "static inline float totk_ssao_tap(texture2d<float> t, sampler s, float2 uv)\n"
    "{\n"
    "    return t.sample(s, select(uv, float2(0.0), isnan(uv))).x;\n"
    "}\n"
    "// Offset of a pair with base vector k = (along, across) rotated by the noise direction (nx, ny); x is scaled by asp.\n"
    "// Same expressions as stock _873/_871 (pair 0) and _877/_875 (pair 1).\n"
    "static inline float2 totk_ssao_off(float nx, float ny, float asp, float2 k)\n"
    "{\n"
    "    float ay = asp * ny;\n"
    "    float ax = asp * nx;\n"
    "    return float2(fma(ax, k.x, ay * -k.y), fma(ny, k.x, nx * k.y));\n"
    "}\n"
    "// One pair (tm = depth at centre - offset, tp = centre + offset); returns (A, B) exactly as stock _1021/_1023.\n"
    "static inline float2 totk_ssao_pair(float c, float tm, float tp, float K, float F, float g, float4 q0, float4 q1)\n"
    "{\n"
    "    float dm = fma(c, q0.w, -tm);\n"
    "    float dp = fma(c, q0.w, -tp);\n"
    "    float sm = fast::clamp(fma(K * dm, g, 0.5), 0.0, 1.0);\n"
    "    float sp = fast::clamp(fma(K * dp, g, 0.5), 0.0, 1.0);\n"
    "    float fm = fast::clamp(fma(F, -dm, q1.y), 0.0, 1.0);\n"
    "    float fp = fast::clamp(fma(F, -dp, q1.y), 0.0, 1.0);\n"
    "    float am = fma(-sm + 0.5, fm, q1.z);\n"
    "    float ap = fma(-sp + 0.5, fp, q1.z);\n"
    "    return float2(fma(fp, sp + -am, am), fma(fm, sm + -ap, ap));\n"
    "}\n"
    "static inline float totk_ssao_botw(float stock, float4 p, float4 q0, float4 q1, float4 q2, float c3_16x,\n"
    "                                   texture2d<float> dt, sampler ds, float u, float v, float nzx, float nzw, float asp,\n"
    "                                   float c, float t0m, float t0p, float t1m, float t1p)\n"
    "{\n"
    "    p = select(p, float4(0.0), isnan(p));\n"
    "    if (all(p == float4(0.0))) return stock;\n"
    "    const float w0 = 0.2545439898967742919921875;            // stock inner-pair weight (literal)\n"
    "    const float w1 = q2.y;                                     // stock outer-pair weight c1[2].y\n"
    "    const float s = p.x > 0.0 ? min(p.x, 8.0) : 1.0;\n"
    "    const float r = p.y > 0.0 ? clamp(p.y, 0.25, 4.0) : 1.0;\n"
    "    const int n = int(clamp(rint(p.z), 0.0, 3.0));   // extra rotated sets (BotW: at most 3)\n"
    "    const float K = c3_16x * 20.0;                             // stock _911\n"
    "    const float F = c3_16x * 0.833333313465118408203125;       // stock _939\n"
    "    const float nx = fma(nzx, 2.0, -1.0);                      // stock _857\n"
    "    const float ny = fma(nzw, 2.0, -1.0);                      // stock _855\n"
    "    float2 k0 = float2(q0.z, 0.00199665012769401073455810546875);   // inner pair, |k| 0.0200 at 5.7 deg\n"
    "    float2 k1 = float2(q1.w, 0.0364835001528263092041015625);       // outer pair, |k| 0.0367 at 95.7 deg\n"
    "    if (r != 1.0) {\n"
    "        k0 *= r;\n"
    "        k1 *= r;\n"
    "        float2 o0 = totk_ssao_off(nx, ny, asp, k0);\n"
    "        float2 o1 = totk_ssao_off(nx, ny, asp, k1);\n"
    "        t0m = totk_ssao_tap(dt, ds, float2(u + -o0.x, v + -o0.y));\n"
    "        t0p = totk_ssao_tap(dt, ds, float2(u + o0.x, v + o0.y));\n"
    "        t1m = totk_ssao_tap(dt, ds, float2(u + -o1.x, v + -o1.y));\n"
    "        t1p = totk_ssao_tap(dt, ds, float2(u + o1.x, v + o1.y));\n"
    "    }\n"
    "    float2 P0 = totk_ssao_pair(c, t0m, t0p, K, F, q1.x, q0, q1);   // gain c1[1].x\n"
    "    float2 P1 = totk_ssao_pair(c, t1m, t1p, K, F, q2.x, q0, q1);   // gain c1[2].x\n"
    "    float sum = fma(P1.x, w1, P0.x * w0) + fma(P1.y, w1, P0.y * w0);   // stock _1037 + _1039 = _1041\n"
    "    float occ = fast::clamp((sum + -0.4321855008602142333984375) * 6.0 * s, 0.0, 1.0);\n"
    "    if (n > 0) {   // m extra full stock pair sets rotated by 90 b / (m + 1) deg; occlusion averaged after the threshold / clamp\n"
    "        const int m = n;\n"
    "        for (int b = 1; b <= m; b++) {\n"
    "            const float a = 1.57079632679 * float(b) / float(m + 1);\n"
    "            const float cr = cos(a), sr = sin(a);\n"
    "            const float rx = fma(nx, cr, -(ny * sr)); const float ry = fma(nx, sr, ny * cr);\n"
    "            float2 o0 = totk_ssao_off(rx, ry, asp, k0), o1 = totk_ssao_off(rx, ry, asp, k1);\n"
    "            float2 A = totk_ssao_pair(c, totk_ssao_tap(dt, ds, float2(u - o0.x, v - o0.y)), totk_ssao_tap(dt, ds, float2(u + o0.x, v + o0.y)), K, F, q1.x, q0, q1);\n"
    "            float2 B = totk_ssao_pair(c, totk_ssao_tap(dt, ds, float2(u - o1.x, v - o1.y)), totk_ssao_tap(dt, ds, float2(u + o1.x, v + o1.y)), K, F, q2.x, q0, q1);\n"
    "            float sb = fma(B.x, w1, A.x * w0) + fma(B.y, w1, A.y * w0);\n"
    "            occ += fast::clamp((sb + -0.4321855008602142333984375) * 6.0 * s, 0.0, 1.0);\n"
    "        }\n"
    "        occ *= 1.0 / float(m + 1);\n"
    "    }\n"
    "    return -occ + 1.0;\n"
    "}\n";
static const struct { const char *prefix; int slot; const char *helper, *store, *call; } k_ssao_keys[] = {
    {"9a5e3335514e4cbb", 48, k_ssao_helper_totk,
     "_455 = as_type<int>(_2366);",
     "_455 = as_type<int>(totk_ssao_ao(_2366, _2358, (*spvDescriptorSet0.fp_c1)._m0[48], float2(_1870, _88), float2(_1874, _1872), float2(_1900, _1902), float3(_252, _248, _232), float3(_439, _427, _431), (*spvDescriptorSet0.fp_c4)._m0[16].x, (*spvDescriptorSet0.fp_c4)._m0[14].x, (*spvDescriptorSet0.fp_c1)._m0, spvDescriptorSet0.fp_t_tcb_A, spvDescriptorSet0.fp_t_tcb_ASmplr));"},
    {"abbbf48f9b5e6598", 48, k_ssao_helper_totk,
     "_461 = as_type<int>(_2370);",
     "_461 = as_type<int>(totk_ssao_ao(_2370, _2362, (*spvDescriptorSet0.fp_c1)._m0[48], float2(_1874, _92), float2(_1878, _1876), float2(_1904, _1908), float3(_252, _244, _228), float3(_433, _437, _441), (*spvDescriptorSet0.fp_c4)._m0[16].x, (*spvDescriptorSet0.fp_c4)._m0[14].x, (*spvDescriptorSet0.fp_c1)._m0, spvDescriptorSet0.fp_t_tcb_A, spvDescriptorSet0.fp_t_tcb_ASmplr));"},
    /* TotK 1.2.1: the same two shaders as 1.4.3 with new keys (the 1.4.3 rebuild kept the compiled code, only the NVN
       control section's metadata changed), so their translations and every anchor here are identical to the pair above:
       checked against all four seed sources of the two 1.2.1 keys (v121 seed, chain v8) and the c1 data (tap offsets
       c1[34].y .. c1[36].x = +/-0.005 .. +/-0.035, c1[48] zero-filled, the store line exactly once per source). */
    {"dd93068627010533", 48, k_ssao_helper_totk,
     "_455 = as_type<int>(_2366);",
     "_455 = as_type<int>(totk_ssao_ao(_2366, _2358, (*spvDescriptorSet0.fp_c1)._m0[48], float2(_1870, _88), float2(_1874, _1872), float2(_1900, _1902), float3(_252, _248, _232), float3(_439, _427, _431), (*spvDescriptorSet0.fp_c4)._m0[16].x, (*spvDescriptorSet0.fp_c4)._m0[14].x, (*spvDescriptorSet0.fp_c1)._m0, spvDescriptorSet0.fp_t_tcb_A, spvDescriptorSet0.fp_t_tcb_ASmplr));"},
    {"67acb7c322ece44d", 48, k_ssao_helper_totk,
     "_461 = as_type<int>(_2370);",
     "_461 = as_type<int>(totk_ssao_ao(_2370, _2362, (*spvDescriptorSet0.fp_c1)._m0[48], float2(_1874, _92), float2(_1878, _1876), float2(_1904, _1908), float3(_252, _244, _228), float3(_433, _437, _441), (*spvDescriptorSet0.fp_c4)._m0[16].x, (*spvDescriptorSet0.fp_c4)._m0[14].x, (*spvDescriptorSet0.fp_c1)._m0, spvDescriptorSet0.fp_t_tcb_A, spvDescriptorSet0.fp_t_tcb_ASmplr));"},
    {"0eeb23f2b287a638", 3, k_ssao_helper_botw,
     "_302 = as_type<int>(_1051);",
     "_302 = as_type<int>(totk_ssao_botw(_1051, (*spvDescriptorSet0.fp_c1)._m0[3], (*spvDescriptorSet0.fp_c1)._m0[0], (*spvDescriptorSet0.fp_c1)._m0[1], (*spvDescriptorSet0.fp_c1)._m0[2], (*spvDescriptorSet0.fp_c3)._m0[16].x, spvDescriptorSet0.fp_t_tcb_20, spvDescriptorSet0.fp_t_tcb_20Smplr, _853, _851, _847, _849, _839, _78, _891, _893, _905, _909));"},
};
static int ssao_entry(const char *key) {
    for (int i = 0; i < (int)(sizeof k_ssao_keys / sizeof *k_ssao_keys); i++) if (!strncmp(key, k_ssao_keys[i].prefix, 16)) return i;
    return -1;
}
static NSString *ssao_patch_src(NvShader *o, NSString *src, int i, const char **why) {
    const int slot = k_ssao_keys[i].slot;
    if ([src rangeOfString:[NSString stringWithFormat:@"_c1)._m0[%d]", slot]].location != NSNotFound) { *why = "the stock source reads the parameter slot"; return nil; }
    for (int b = 0; b < 16; b++) if (o->rf.c1[slot * 16 + b]) { *why = "the parameter slot holds constant data"; return nil; }
    NSString *entry = @"fragment main0_out main0(", *store = @(k_ssao_keys[i].store);
    if ([src componentsSeparatedByString:entry].count != 2) { *why = "the fragment entry is not there exactly once"; return nil; }
    if ([src componentsSeparatedByString:store].count != 2) { *why = "the AO store line is not there exactly once"; return nil; }
    /* Chain v8 (upstream ae8f4f4) puts [[ early_fragment_tests ]] before the entry of flagged shaders, the AO ones among
       them: the helpers go before the attribute, which would otherwise apply to the first helper (a compile error). */
    NSString *efe = [@"[[ early_fragment_tests ]] " stringByAppendingString:entry];
    NSString *at = [src rangeOfString:efe].location != NSNotFound ? efe : entry;
    NSString *out = [src stringByReplacingOccurrencesOfString:at withString:[NSString stringWithFormat:@"%s\n%@", k_ssao_keys[i].helper, at]];
    out = [out stringByReplacingOccurrencesOfString:store withString:@(k_ssao_keys[i].call)];
    return [@SSAO_MARK stringByAppendingString:out];
}

/* shader_load, for a fragment shader with a c1 while TOTK_SSAO=1: one of k_ssao_keys gets its patched source compiled into
   o->fn and o->ssao_slot set, and the patched source is returned (the scans after it -- depth output, driver reads, stores,
   c1 sharing -- and the variants made from it then see it); any other shader, or a failed patch, returns src. */
static NSString *ssao_patch(NvShader *o, NSString *src, NSString *entry_name) {
    int i = ssao_entry(o->key); if (i < 0) return src;
    const char *why = "no MSL source (a precompiled library)"; NSError *pe = nil; uint64_t t0 = bp_now();
    NSString *ps = src ? ssao_patch_src(o, src, i, &why) : nil;
    id<MTLLibrary> pl = ps ? totk_msl_library(ps, &pe) : nil;   // not variant_library: never recorded into the packaged variants
    id<MTLFunction> pf = pl ? [pl newFunctionWithName:entry_name] : nil;
    if (!pf) { MLOG("【红】[ssao] %s: %s; this shader stays stock", o->key, ps ? (pe ? pe.localizedDescription.UTF8String : "no entry function") : why); return src; }
    o->fn = pf; o->ssao_slot = k_ssao_keys[i].slot;
    MLOG("[ssao] %s patched: parameters in c1[%d], compiled in %.0f ms", o->key, o->ssao_slot, (bp_now() - t0) / 1e6);
    return ps;
}

# Hyrule Warriors: Age of Calamity (01002B00111A2000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from functools import partial
from nso_asm import ADRP, B
SOUND_HEAPS = ('sound engine heap: mov x8,#0xf00000 → mov x8,#0x1300000 (+4 MB)',
               'sound data heap: mov w9,#0x8e00000 → mov w9,#0x8a00000 (-4 MB; the block the three heaps come from keeps its total size)')
CUTS = {
  # Hyrule Warriors: Age of Calamity 1.3.0 (v327680) main: the init (main+0x8d9610) hands the KTSL sound engine three heaps carved from one block: 15 MB engine memory, 2 MB, and
  # 142 MB sound data (sizes built by mov x8,#0xf00000; movk x8,#0x20,lsl #48 and mov w9,#0x8e00000; constructor main+0x12647a0, heaps created at main+0x1a2ba58). The audio renderer
  # work buffer in the 15 MB heap is 0x760000 (the SDK's AudioRenderSystem::GetWorkBufferSize, 0x40a9c8 of it command buffer), and the engine's setup asks for about 16.9 MB in
  # all: the fourth 0x1b5860-byte player block (main+0x1a32f70; three of the four players mix 5.1) came back null, the setup released its memory pools and gave up (-51),
  # the first sound archive (ASRS) never loaded and nnMain's resource wait (main+0x8d9bc8) spun forever. Where hardware differs is not known. The engine heap gets 4 MB from the
  # sound data heap, so the block the three come from keeps its size
  'c3cf52bf2b05d7310884971b50bcccb8': [
    (0x8d9610, '081ea0d2', '0826a0d2', SOUND_HEAPS[0]),
    (0x8d9618, '091ca152', '0914a152', SOUND_HEAPS[1]),
  ],
  # 1.4.1 (v458752) main: the same instructions, 0x630 further on.
  'e5f275b5d0889899e55de1a164eef060': [
    (0x8d9c40, '081ea0d2', '0826a0d2', SOUND_HEAPS[0]),
    (0x8d9c48, '091ca152', '0914a152', SOUND_HEAPS[1]),
    # Output 1440p: output 2560x1440, scene 1920x1080 (1.4.1 only; 1.3.0 keeps 1920x1080 and 1440x810). nnMain builds the KTGL device config with x8 = {1920, 1080}
    # and stores it twice, as config[0..1] (window crop, device +0x2a90) and config[8..9] (output depth, device +0x2ab0).
    # The swap-chain textures take config[0..1] only when device +0x2a32 is 0; on the Mac it is not, and the init's csel
    # picks its own 1920x1080 constants (main+0x17c5de0/0x17c5df4), so those change too. After the init, nnMain stores
    # the scene size {1440, 810} at [app + 0x368]. The UI keeps its 1920x1080 reference canvas and scales by output / 1920.
    (0x8d9ac0, '08f080d2', '084081d2', 'output width: mov x8,#0x780 -> mov x8,#0xa00 (2560)'),
    (0x8d9ac8, '0887c0f2', '08b4c0f2', 'output height: movk x8,#0x438,lsl #32 -> movk x8,#0x5a0,lsl #32 (1440)'),
    (0x8d9b00, '08b480d2', '08f080d2', 'scene width: mov x8,#0x5a0 -> mov x8,#0x780 (1920)'),
    (0x8d9b04, '4865c0f2', '0887c0f2', 'scene height: movk x8,#0x32a,lsl #32 -> movk x8,#0x438,lsl #32 (1080)'),
    (0x17c5de0, 'ea0f1932', '0a408152', 'swap-chain width when +0x2a32 is set: mov w10,#0x780 -> mov w10,#0xa00 (2560)'),
    (0x17c5df4, '0b878052', '0bb48052', 'swap-chain height when +0x2a32 is set: mov w11,#0x438 -> mov w11,#0x5a0 (1440)'),
  ],
}

# Where each build keeps what the stubs below touch. Addresses are 1.3.0's in the
# comments of this file. 1.4.1 was mapped by matching the code that reads each one
# and checked by disassembly: the instructions are the same, except that the scene
# site reads the frame-rate block at its new offset. The device fields (+0x2aa4,
# +0x32d0) stay where they were.
#   task, step, device, scene: the four stub sites; scene_word: the scene site's original
#   instruction; rate: the {fps, interval, fps, float fps} block; ntsc: the 59.94 float;
#   ktgl: the pointer to the KTGL device.
LAYOUT_130 = dict(task=0x166dab4, step=0x166dc60, device=0x8d94b0, scene=0x8c3610, scene_word='295d41bd',
                  rate=0x269e150, ntsc=0x1ef5f3c, ktgl=0x2bac798)
# 1.4.1: the camera aspect stores (str s<n>,[x<m>,#0x12ec]) the aspect-ratio stubs below wrap, with their original bytes.
ASPECT_STORES = {0xb0cf48: 'a1ee12bd', 0xb0e5d0: '61ee12bd', 0xb0e934: 'a1ee12bd', 0xfa85f8: '62ee12bd',
                 0xfbf99c: '82ee12bd', 0x115fa64: '00ec12bd'}
LAYOUT_141 = dict(task=0x1670124, step=0x16702d0, device=0x8d9ae0, scene=0x8c3c40, scene_word='298d42bd',
                  rate=0x26a2280, ntsc=0x1ef8ddc, ktgl=0x2bb0798)

# AoC 1.3.0 has three independent clocks: KIDS task, battle float-fps,
# and KTGL event/device rate. At +0x166dc60, x0 is the post-wait tick,
# x21 the previous tick and x24 the frequency. s1 excludes the wait: never use
# that work-only interval as a simulation step. Use (x0-x21)/x24 instead,
# preserving the frame object's timescale. No present-thread estimate feeds the
# simulation. The battle/device clocks take the preceding task's bounded step.
# Integer fps readers get the nearest rate; the battle caller at +0x8c3610 reads
# the exact reciprocal float. The task limiter always retains the selected cap.
# State ABI (loader.c/aoc_timing.h): +0 target, +0x38 dynamic, +0x3c dt,
# +0x40 accumulated task seconds, +0x48 updates, +0x50 max dt, +0x54 min dt,
# +0x58 previous target. Long gaps are capped at 100 ms instead of replayed.
# x8/x16/x17, s16-s18 and flags are dead at these asserted sites.
# These patches do not cover the game's integer 60 Hz tick consumers of
# main+0x269e154 (movement/effects/script countdowns). At 120 updates/sec,
# interval 1 advances those consumers twice as fast. The host consequently
# restricts targets to 30/60; correct task/scene counters do not validate 120.
def frame_step(ctx, at=LAYOUT_130):
    ctx.aoc_timing_abi = 3
    here, emit, state = ctx.here, ctx.emit, ctx.state
    task, step, device, scene = at['task'], at['step'], at['device'], at['scene']
    rate, ntsc, ktgl = at['rate'], at['ntsc'], at['ktgl']
    rate_lo, scene_word = rate & 0xfff, int.from_bytes(bytes.fromhex(at['scene_word']), 'little')
    ctx.site(task, '68734339')
    ctx.site(step, 'e83640b9681a00b9e04640bd')
    ctx.site(device, '4900c0f2')
    ctx.site(scene, at['scene_word'])

    # Local label assembler: avoids fragile hand-counted branch displacements.
    def stub(site, resume, words, reason):
        start = here(); labels = {}; pc = start
        for word in words:
            if isinstance(word, str): labels[word] = pc
            else: pc += 4
        for word in words:
            if isinstance(word, str): continue
            if isinstance(word, tuple):
                op, arg = word
                if op == 'page': word = ADRP(arg[0], here(), arg[1])
                elif op == 'b': word = B(here(), labels[arg])
                elif op == 'zero64':
                    name, reg = arg
                    word = 0xb4000000 | (((labels[name] - here()) // 4 & 0x7ffff) << 5) | reg
                elif op.startswith('cond'):
                    word = 0x54000000 | (((labels[arg] - here()) // 4 & 0x7ffff) << 5) | int(op[4:])
                elif op.startswith('cbz'):
                    word = 0x34000000 | (((labels[arg] - here()) // 4 & 0x7ffff) << 5) | int(op[3:])
                elif op == 'bl':   # a call to an absolute target
                    word = 0x80000000 | B(here(), arg)
            emit(word)
        emit(B(here(), resume))
        ctx.put(site, [B(site, start)], reason)

    def ld(base, reg, off, kind='s'):
        return {'s':0xbd400000, 'w':0xb9400000, 'd':0xfd400000, 'x':0xf9400000}[kind] | ((off // (8 if kind in ('d','x') else 4)) << 10) | (base << 5) | reg
    def st(base, reg, off, kind='s'):
        return ld(base, reg, off, kind) - 0x00400000
    page = ('page', (16, state))
    stub(device, device + 4, [page, ld(16,16,0,'w'),
        0x52a83e11, 0x6b11021f, ('cond13','two'), 0xf2c00029, ('b','done'),
        'two', 0xf2c00049, 'done'], 'AoC device interval: 1 above 30 fps, else original 2')

    stub(task, task + 4, [page, ld(16,16,0),
        0x1e202208, ('cond13','native'),             # fcmp target,0; b.le
        0x1e380208, st(19,8,0x30,'w'),             # task limiter = cap
        0x1e2e1012, 0x1e301a52,                    # s18 = 1 / cap
        ld(16,8,0x38,'w'), ('cbz8','dt'),
        ld(16,17,0x3c),
        ld(16,8,0x74,'w'), ('cbz8','source'), ld(16,17,0x70),
        'source', 0x1e202228, ('cond13','dt'),       # previous scene dt, else task dt
        ld(16,8,0x58,'w'), 0x1e260211, 0x6b11011f, ('cond1','dt'),
        0x1e204232,                               # s18 = previous dt
        'dt', st(16,16,0x58),
        0x1e2e1011, 0x1e321a30,                    # s16 = reciprocal dt
        0x1e200208,                               # integer rate (nearest)
        ('b','block'),
        'native', 0x528003c8, st(19,8,0x30,'w'),   # restore 30 on live switch
        0x1e230110,                               # ucvtf s16,w8
        st(16,31,0x58,'w'), st(16,31,0x74,'w'), st(16,31,0x68,'x'),
        'block', ('page',(17,rate)),
        st(17,8,rate_lo,'w'), st(17,8,rate_lo + 8,'w'), st(17,16,rate_lo + 12),
        ld(16,8,0,'w'), 0x52a83e11, 0x6b11011f,  # target bits <= 30 bits
        0x1a9fc7e8, 0x11000508,                    # interval = 2 at 30, else 1
        ('page',(17,rate)), st(17,8,rate_lo + 4,'w'),
        # Device events retain the original 59.94/60 convention at any rate.
        ('page',(8,ntsc)), ld(8,17,ntsc & 0xfff),
        0x52800788, 0x1e230112, 0x1e321a31, 0x1e310a10,
        ('page',(17,ktgl)), ld(17,17,ktgl & 0xfff,'x'),
        # cbz x17 (64-bit; local cbz helper's w encoding is patched here)
        # device is initialized before the frame task starts. Still tolerate null.
        0xb5000051, ('b','orig'),                   # cbnz x17,+8
        st(17,16,0x32d0),
        ld(16,8,0,'w'), 0x52a83e10, 0x6b10011f, # target <= 30
        0x1a9fc7e8, 0x11000508, st(17,8,0x2aa4,'w'), # live device interval
        'orig', 0x39437368], 'AoC task cap and dynamic battle/event clocks (30/60/120)')

    stub(step, step + 12, [0xbd4036f0, page,
        ld(16,8,0,'w'), ('cbz8','original'),
        ld(16,17,0), 0x1e2e1012,                  # s18=1/target
        0x1e311a52,                               # fdiv s18,s18,s17
        ld(16,8,0x38,'w'), ('cbz8','measured'),
        0xcb150011, 0x9e630231, 0x9e630312,       # x17=x0-x21; d17=elapsed; d18=frequency
        0x1e721a32, 0x1e624252,                   # s18 = full elapsed seconds after wait
        ld(16,17,0x50), 0x1e315a52,              # min(dt,100ms)
        ld(16,17,0x54), 0x1e314a52,              # max(dt,1ms)
        'measured', st(16,18,0x3c),
        ld(23,8,0x48,'w'), 0x1e230111,            # object's fps -> s17
        0x1e310a51,                               # s17=dt*object fps
        ld(23,0,0x44), 0x1e310a10, 0x1e310800,
        ('b','out'),
        'original', ld(23,0,0x44),
        'out', 0x1e22c211, ld(16,18,0x40,'d'), 0x1e712a52, st(16,18,0x40,'d'),
        ld(16,17,0x48,'x'), 0x91000631, st(16,17,0x48,'x'),
        0x1e260208, st(19,8,0x18,'w')], 'AoC KIDS clock: measured bounded dt times original timescale, or fixed target')

    # Scene/battle updates can run less often than the independent KIDS task.
    # Measure their own wall interval instead of reusing a 120 Hz task's 8.3 ms
    # when battle rendering only reaches e.g. 48 Hz. CNTVCT is the native Mac
    # counter, converted using native CNTFRQ_EL0 supplied by the host (+0x60).
    # On M5 this is 1 GHz; the 24 MHz mach_absolute_time ABI is a different clock.
    # x9 is overwritten by the original next instruction; x16/x17 and s16-s18
    # are scratch before the next call. s9 replaces the original fps load.
    stub(scene, scene + 4, [page, ld(16,16,0), 0x1e202208, ('cond13','original'),
        0x1e2e1012, 0x1e301a52,                  # default dt=1/target
        0xd53be051, ld(16,9,0x68,'x'), st(16,17,0x68,'x'),
        ld(16,17,0x38,'w'), ('cbz17','dt'),
        0xd53be051, # reload native tick after using w17 for mode
        ('zero64',('dt',9)), 0xcb090231, 0x9e230232,
        ld(16,17,0x60), 0x1e310a52,             # seconds since this scene's preceding update
        ld(16,17,0x50), 0x1e315a52, ld(16,17,0x54), 0x1e314a52,
        'dt', st(16,18,0x70), 0x52800029, st(16,9,0x74,'w'),
        0x1e2e1011, 0x1e321a29,                # s9=1/dt, consumed by battle's own reciprocal
        0x1e200129, ('page',(17,rate)),
        st(17,9,rate_lo,'w'), st(17,9,rate_lo + 8,'w'), st(17,9,rate_lo + 12),
        0x1e22c251, ld(16,18,0x80,'d'), 0x1e712a52, st(16,18,0x80,'d'),
        ld(16,17,0x78,'x'), 0x91000631, st(16,17,0x78,'x'),
        ('b','done'), 'original', scene_word, 'done'],
        'AoC scene/battle clock: own measured interval, independent of KIDS/present rates')

    # Output 1440p, 1.4.1: the UI 2D primitive node (minimap markers). The UI renderer
    # main+0x18f8ca0 draws these nodes in 1920x1080 canvas pixels through KTGL's 2D pipeline, whose
    # ortho is (2*sx/W, -2*sy/H) with W,H the viewport and sx,sy at dev+0x28b0/+0x28b4 (1.0).
    # When the viewport is the output, set sx,sy = output/1920x1080 around the node; restore 1.0.
    if at is LAYOUT_141:
        begin2d, node, after = 0x18f8f30, 0x18f969c, 0x17e33b0
        ctx.site(begin2d, '20a9fb97')   # bl 0x17e33b0
        ctx.site(node, 'a10303d1')
        stub(begin2d, begin2d + 4, [
            0xb9802728,                    # ldrsw x8,[x25,#0x24]   viewport index
            0xf100011f, ('cond11','call'),  # cmp x8,#0; b.lt
            0xf9400f29, 0x52800b0a,        # ldr x9,[x25,#0x18]; mov w10,#0x58
            0x9b0a2508, 0xf9402908,        # madd x8,x8,x10,x9; ldr x8,[x8,#0x50]
            0xb9407909,                    # ldr w9,[x8,#0x78]      viewport width
            0xb96a932a,                    # ldr w10,[x25,#0x2a90]  output width
            0x6b0a013f, ('cond1','call'),   # cmp w9,w10; b.ne
            0x1e220150, 0x52a89e0b, 0x1e270171, 0x1e311a10,   # s16 = output w / 1920
            0xb96a972a,                    # ldr w10,[x25,#0x2a94]  output height
            0x1e220152, 0x52a890eb, 0x1e270171, 0x1e311a52,   # s18 = output h / 1080
            0xbd28b330, 0xbd28b732,        # str s16,[x25,#0x28b0]; str s18,[x25,#0x28b4]
            'call', ('bl', after)],
            'UI 2D node in canvas pixels: dev 2D scale = output/1920x1080 when drawing to the output')
        stub(node, node + 4, [
            0x52a7f010, 0xb928b330, 0xb928b730,   # w16 = 1.0f; dev 2D scale back to 1.0
            0xd10303a1],                          # sub x1,x29,#0xc0 (original)
            'UI 2D node end: dev 2D scale back to 1.0')

        # Aspect ratio (16:10 like TOTK, 1.4.1): the canvas stays 16:9 and the host stretches it to the chosen shape, so
        # each camera's aspect (camera +0x12ec = scene w/h times its split factor) is multiplied by the state's factor
        # (+0x88: chosen ratio / (16/9); 0 = the game's own). Only 3D views of 16:9 or wider (>= 1.5) are touched, which
        # covers the split-screen halves (32:9) and leaves square offscreen cameras alone. Six writers compute the aspect
        # from a view size and then call the camera's update (vtable +0x418): five from the scene size at [app+0x368],
        # one (main+0x115fa64) from its own size at +0xf0; that one reads its factor from +0x8c, so the host can turn it
        # off alone. Every site is followed by a call or a tail call, so x16/x17 and s16/s17 are free there.
        for site, slot in ((0xb0cf48, 0x88), (0xb0e5d0, 0x88), (0xb0e934, 0x88), (0xfa85f8, 0x88), (0xfbf99c, 0x88),
                           (0x115fa64, 0x8c)):
            orig = ASPECT_STORES[site]
            ctx.site(site, orig)
            w = int.from_bytes(bytes.fromhex(orig), 'little')
            src, base = w & 31, (w >> 5) & 31
            stub(site, site + 4, [page, ld(16,16,slot),
                0x1e202208, ('cond13','orig'),             # fcmp s16,#0.0; b.le: the game's own aspect
                0x1e2f1011, 0x1e312000 | src << 5,         # fmov s17,#1.5; fcmp s<src>,s17
                ('cond11','orig'),                         # b.lt: narrower than 3:2, not the screen's view
                0x1e300811 | src << 5,                     # fmul s17,s<src>,s16
                st(base,17,0x12ec), ('b','done'),
                'orig', w, 'done'],
                f'camera aspect times the chosen ratio over 16:9 (state +{slot:#x})')

FRAME_STEP = {'c3cf52bf2b05d7310884971b50bcccb8': frame_step,
              'e5f275b5d0889899e55de1a164eef060': partial(frame_step, at=LAYOUT_141)}

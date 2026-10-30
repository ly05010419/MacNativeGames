# Hyrule Warriors: Age of Calamity (01002B00111A2000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's NSO build ID;
# CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B
CUTS = {
  # Hyrule Warriors: Age of Calamity 1.3.0 (v327680) main: the init (main+0x8d9610) hands the KTSL sound engine three heaps carved from one block: 15 MB engine memory, 2 MB, and
  # 142 MB sound data (sizes built by mov x8,#0xf00000; movk x8,#0x20,lsl #48 and mov w9,#0x8e00000; constructor main+0x12647a0, heaps created at main+0x1a2ba58). The audio renderer
  # work buffer in the 15 MB heap is 0x760000 (the SDK's AudioRenderSystem::GetWorkBufferSize, 0x40a9c8 of it command buffer), and the engine's setup asks for about 16.9 MB in
  # all: the fourth 0x1b5860-byte player block (main+0x1a32f70; three of the four players mix 5.1) came back null, the setup released its memory pools and gave up (-51),
  # the first sound archive (ASRS) never loaded and nnMain's resource wait (main+0x8d9bc8) spun forever. Where hardware differs is not known. The engine heap gets 4 MB from the
  # sound data heap, so the block the three come from keeps its size
  'c3cf52bf2b05d7310884971b50bcccb8': [
    (0x8d9610, '081ea0d2', '0826a0d2', 'sound engine heap: mov x8,#0xf00000 → mov x8,#0x1300000 (+4 MB)'),
    (0x8d9618, '091ca152', '0914a152', 'sound data heap: mov w9,#0x8e00000 → mov w9,#0x8a00000 (-4 MB; the block the three heaps come from keeps its total size)'),
  ],
}

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
def frame_step(ctx):
    ctx.aoc_timing_abi = 3
    here, emit, state = ctx.here, ctx.emit, ctx.state
    ctx.site(0x166dab4, '68734339')
    ctx.site(0x166dc60, 'e83640b9681a00b9e04640bd')
    ctx.site(0x8d94b0, '4900c0f2')
    ctx.site(0x8c3610, '295d41bd')

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
            emit(word)
        emit(B(here(), resume))
        ctx.put(site, [B(site, start)], reason)

    def ld(base, reg, off, kind='s'):
        return {'s':0xbd400000, 'w':0xb9400000, 'd':0xfd400000, 'x':0xf9400000}[kind] | ((off // (8 if kind in ('d','x') else 4)) << 10) | (base << 5) | reg
    def st(base, reg, off, kind='s'):
        return ld(base, reg, off, kind) - 0x00400000
    page = ('page', (16, state))
    stub(0x8d94b0, 0x8d94b4, [page, ld(16,16,0,'w'),
        0x52a83e11, 0x6b11021f, ('cond13','two'), 0xf2c00029, ('b','done'),
        'two', 0xf2c00049, 'done'], 'AoC device interval: 1 above 30 fps, else original 2')

    stub(0x166dab4, 0x166dab8, [page, ld(16,16,0),
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
        'block', ('page',(17,0x269e150)),
        st(17,8,0x150,'w'), st(17,8,0x158,'w'), st(17,16,0x15c),
        ld(16,8,0,'w'), 0x52a83e11, 0x6b11011f,  # target bits <= 30 bits
        0x1a9fc7e8, 0x11000508,                    # interval = 2 at 30, else 1
        ('page',(17,0x269e150)), st(17,8,0x154,'w'),
        # Device events retain the original 59.94/60 convention at any rate.
        ('page',(8,0x1ef5f3c)), ld(8,17,0xf3c),
        0x52800788, 0x1e230112, 0x1e321a31, 0x1e310a10,
        ('page',(17,0x2bac798)), ld(17,17,0x798,'x'),
        # cbz x17 (64-bit; local cbz helper's w encoding is patched here)
        # device is initialized before the frame task starts. Still tolerate null.
        0xb5000051, ('b','orig'),                   # cbnz x17,+8
        st(17,16,0x32d0),
        ld(16,8,0,'w'), 0x52a83e10, 0x6b10011f, # target <= 30
        0x1a9fc7e8, 0x11000508, st(17,8,0x2aa4,'w'), # live device interval
        'orig', 0x39437368], 'AoC task cap and dynamic battle/event clocks (30/60/120)')

    stub(0x166dc60, 0x166dc6c, [0xbd4036f0, page,
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
    stub(0x8c3610, 0x8c3614, [page, ld(16,16,0), 0x1e202208, ('cond13','original'),
        0x1e2e1012, 0x1e301a52,                  # default dt=1/target
        0xd53be051, ld(16,9,0x68,'x'), st(16,17,0x68,'x'),
        ld(16,17,0x38,'w'), ('cbz17','dt'),
        0xd53be051, # reload native tick after using w17 for mode
        ('zero64',('dt',9)), 0xcb090231, 0x9e230232,
        ld(16,17,0x60), 0x1e310a52,             # seconds since this scene's preceding update
        ld(16,17,0x50), 0x1e315a52, ld(16,17,0x54), 0x1e314a52,
        'dt', st(16,18,0x70), 0x52800029, st(16,9,0x74,'w'),
        0x1e2e1011, 0x1e321a29,                # s9=1/dt, consumed by battle's own reciprocal
        0x1e200129, ('page',(17,0x269e150)),
        st(17,9,0x150,'w'), st(17,9,0x158,'w'), st(17,9,0x15c),
        0x1e22c251, ld(16,18,0x80,'d'), 0x1e712a52, st(16,18,0x80,'d'),
        ld(16,17,0x78,'x'), 0x91000631, st(16,17,0x78,'x'),
        ('b','done'), 'original', 0xbd415d29, 'done'],
        'AoC scene/battle clock: own measured interval, independent of KIDS/present rates')

FRAME_STEP = {'c3cf52bf2b05d7310884971b50bcccb8': frame_step}

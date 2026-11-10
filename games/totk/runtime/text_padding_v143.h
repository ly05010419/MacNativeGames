/* 1.4.3 main: who owns which bytes of the padding after the code (main+0x2bb9c00 to the read-only data at main+0x2bba000,
   1024 bytes). Every veneer the 1.4.3 patches write there takes its offset from this table, and the checks below stop the
   build when two of them overlap or one runs past the padding. Before 2026-11-10 each patch kept its own number in a
   comment; the button prompt cache (PR #1940) took +896, inside the Vert+ stubs (+320 to +928), so a VR run, the only
   one that writes the Vert+ stubs, stopped at start ("aspect patch: text padding for Vert+ in use"). A new veneer gets a
   row here. Free: +304 to +320 and +1008 to +1024. */
#pragma once

enum {
    PAD_V143_FRAME_STEP = 0,     PAD_V143_FRAME_STEP_LEN = 16,     /* real-time frame step (patch_realtime) */
    PAD_V143_OUTPUT = 16,        PAD_V143_OUTPUT_LEN = 28,         /* output resolution (patch_output_res) */
    PAD_V143_ASPECT = 48,        PAD_V143_ASPECT_LEN = 4 * 32,     /* ultrawide: 4 sites x ASPECT_STUB */
    PAD_V143_CLOTH = 176,        PAD_V143_CLOTH_LEN = 19 * 4,      /* cloth / hair clock (patch_secondary_animation) */
    PAD_V143_BOUNCE = 256,       PAD_V143_BOUNCE_LEN = 20,         /* trampoline bounce (patch_slowmo_bounce) */
    PAD_V143_PROMPT = 288,       PAD_V143_PROMPT_LEN = 16,         /* button prompt cache (prompt_cache_install) */
    PAD_V143_VERT = 320,         PAD_V143_VERT_LEN = 2 * 304,      /* Vert+: 2 cameras x ASPECT_VSTUB (VR head look) */
    PAD_V143_FADE = 928,         PAD_V143_FADE_LEN = 80,           /* LOD fade increment (patch_fade_increment) */
    PAD_V143_SIZE = 1024,                                          /* main+0x2bb9c00 to main+0x2bba000 */
};

_Static_assert(PAD_V143_FRAME_STEP + PAD_V143_FRAME_STEP_LEN <= PAD_V143_OUTPUT, "1.4.3 padding: frame step overlaps");
_Static_assert(PAD_V143_OUTPUT + PAD_V143_OUTPUT_LEN <= PAD_V143_ASPECT, "1.4.3 padding: output overlaps");
_Static_assert(PAD_V143_ASPECT + PAD_V143_ASPECT_LEN <= PAD_V143_CLOTH, "1.4.3 padding: aspect overlaps");
_Static_assert(PAD_V143_CLOTH + PAD_V143_CLOTH_LEN <= PAD_V143_BOUNCE, "1.4.3 padding: cloth overlaps");
_Static_assert(PAD_V143_BOUNCE + PAD_V143_BOUNCE_LEN <= PAD_V143_PROMPT, "1.4.3 padding: bounce overlaps");
_Static_assert(PAD_V143_PROMPT + PAD_V143_PROMPT_LEN <= PAD_V143_VERT, "1.4.3 padding: prompt cache overlaps");
_Static_assert(PAD_V143_VERT + PAD_V143_VERT_LEN <= PAD_V143_FADE, "1.4.3 padding: Vert+ overlaps");
_Static_assert(PAD_V143_FADE + PAD_V143_FADE_LEN <= PAD_V143_SIZE, "1.4.3 padding: fade runs past the padding");
_Static_assert(PAD_V143_PROMPT % 8 == 0, "1.4.3 padding: the prompt veneer's address literal must be 8-byte aligned");

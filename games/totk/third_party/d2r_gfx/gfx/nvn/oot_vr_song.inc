/* zeldaVR, Ocarina of Time, first person: a song's effects left out of the picture (vr.md row 70; the VR page's "a song's effects
   around you: hidden", off by default). When a song played with the ocarina in the hands succeeds, the game plays its effect inside
   its ocarina scene (Saria's Song: green leaves round Link); from Link's eyes they are round the player's head. HookshotVR's notes
   (vr/items/ocarina/song_effects.inc, GPL-3: the rule and its numbers, no code): while a song plays (the ocarina's scene goes on
   with no note wait and no song list: zeldaVR/gfx/oot_vr_scene_draw.h vr_oot_song_fx_hidden), every effect draw, one whose vertex
   shader reads the particle camera (uniform block 10) and the effect's matrix (block 11), whose effect sits within 4 m of the eye
   is left out; another effect that near (a torch beside Link) is left out for those seconds too. The effect's place is the
   matrix's translation (floats 12-14, as HookshotVR reads the same game data); the eye is the one our camera hook gave the game.
   Not confirmed in a run (no save with an ocarina). Included by oot_vr_body.inc. */
int vr_oot_song_fx_hidden(void) __attribute__((weak_import));
static uint64_t g_oot_song_left_out;
static int oot_song_fx_skip(__unsafe_unretained NvShader *vs) {
    if (!vs || !vr_oot_song_fx_hidden || !vr_oot_song_fx_hidden() || !runtime_vr_base_view) return 0;
    int camera = 0, effect = 0;
    for (int i = 0; i < vs->rf.nb; i++) {
        if (vs->rf.b[i].kind != NVN_RB_CBUF) continue;
        camera |= vs->rf.b[i].reg == 10; effect |= vs->rf.b[i].reg == 11;
    }
    if (!camera || !effect) return 0;
    const p_bind_buf *u = &S.ubo[0][11], *cam = &S.ubo[0][10];
    if (!u->addr || u->size < 64 || u->size > 0x10000 || !cam->addr || cam->size < 0xc0 || cam->size > 0x10000) return 0;
    float m[16], v[12];
    pend_materialize(u->addr, (uint8_t *)m, sizeof m, 0);
    if (!runtime_vr_base_view(v)) return 0;
    float eye[3];
    for (int i = 0; i < 3; i++) eye[i] = -(v[i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]);
    const float d[3] = { m[12] - eye[0], m[13] - eye[1], m[14] - eye[2] }, dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (!(dd < 4.0f * 4.0f)) return 0;
    if (g_oot_song_left_out++ % 2000 == 0) MLOG("[vr-ocarina] a song's effect draw left out (the VR page: hidden; %llu so far; %.2f m from the eye)", (unsigned long long)g_oot_song_left_out, (double)sqrtf(dd));
    return 1;
}

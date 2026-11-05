/* Quest replaces Labo's display optics, while the native world IPD remains.
 * The graphics system writes view+0x404 before UI layout, render-to-texture
 * crops and the native comfort mask read that same screen-parallax value.
 * Zero it at this common producer; changing only GPU CB10 breaks its crops.
 * Verified by a native write/read watch on the menu view, 2026-11-05. */
#pragma once

static void botw_vr_ui_profile_install(Module *m) {
#if defined(GAME_BOTW160)
    if(ios_code()||!vr_labo_enabled())return;
    const struct {uint64_t off;uint32_t word;} sites[]={
        {0x1822470,0xbd040509},{0x1822480,0xbd040509},
        {0x1822490,0xbd040509},{0x1822568,0xbd040509},
        {0x12baf54,0xbd44050d},{0x17ec43c,0xbd440500}
    };
    for(size_t i=0;i<sizeof sites/sizeof *sites;i++){
        uint32_t have=0;if(sites[i].off+4<=m->text_end)memcpy(&have,m->base+sites[i].off,4);
        if(have!=sites[i].word)runtime_fail("BotW native UI optics producer/consumer does not match");
    }
    for(unsigned i=0;i<4;i++)put_word(m,sites[i].off,0xb904051fu,"Quest UI screen parallax: zero at the native producer");
    fprintf(stderr,"[vr] Quest UI optics: native view screen-parallax producer disabled; world eye separation unchanged\n");
#else
    (void)m;
#endif
}

/* Our independent renderer. The graphics system owns a queued display
 * mode (+0x123d/+0x11bd), separate from the Labo option in config+0x10 and its
 * option-request pair. Our VR entry requests same-frame docked stereo only. */
#pragma once
static uint8_t *botw_stereo_main;

static void botw_stereo_install(Module *m) {
    if (ios_code() || !vr_labo_enabled()) return;
#ifdef GAME_BOTW190
    const uint64_t at = 0x1265ae8;
    const uint32_t original[] = {0x528237a8, 0xaa0003f3, 0x1e204008, 0x8b080016};
#else
    const uint64_t at = 0x1821644;
    const uint32_t original[] = {0x39400348, 0x7103fd1f, 0x54001260, 0x71000d1f, 0x39000748};
#endif
    if (at + sizeof original > m->text_end || memcmp(m->base + at, original, sizeof original))
        runtime_fail("BotW graphics display request does not match the original instructions");
#ifdef GAME_BOTW190
    const uint64_t owner_at = 0xd4aa04;
    const uint32_t owner[] = {0xb00087c8, 0x9137c108, 0xf9400108, 0xf941c100};
#else
    const uint64_t owner_at = 0x119f9f0;
    const uint32_t owner[] = {0xf000d968, 0xf9457908, 0xf941c108};
    uint32_t offset_word = 0, pointer_word = 0;
    memcpy(&offset_word, m->base + 0x1821534, 4);
    memcpy(&pointer_word, m->base + 0x18215d4, 4);
    if (offset_word != 0x528247b4 || pointer_word != 0x8b14039a)
        runtime_fail("BotW graphics display request owner/offset does not match the original instructions");
#endif
    if (owner_at + sizeof owner > m->text_end || memcmp(m->base + owner_at, owner, sizeof owner))
        runtime_fail("BotW graphics owner does not match the original instructions");
    botw_stereo_main = m->base;
    fprintf(stderr, "[vr] BotW: our same-frame stereo renderer enabled independently of the game option\n");
}
static void botw_stereo_tick(uint64_t now) {
    if (!botw_stereo_main || !now || ios_code()) return;
    pthread_mutex_lock(&botw_vr_mu);botw_vr_epoch++;pthread_mutex_unlock(&botw_vr_mu);
#ifdef GAME_BOTW190
    const uint64_t root = 0x1e43df0, mode_offset = 0x11bd;
#else
    const uint64_t root = 0x2cceaf0, mode_offset = 0x123d;
#endif
    uintptr_t graphics = 0, config = 0;
    memcpy(&graphics, botw_stereo_main + root, sizeof graphics);
    if (!graphics) return;
    memcpy(&config, (const void *)(graphics + 0x380), sizeof config);
    if (!config) return;
    unsigned char option = 0, pending = 0, current = 0;
    memcpy(&option, (const void *)(config + 0x10), 1);
    memcpy(&pending, (const void *)(graphics + mode_offset), 1);
    memcpy(&current, (const void *)(graphics + mode_offset + 1), 1);
    static unsigned last = ~0u;
    unsigned state = ((unsigned)option << 16) | ((unsigned)current << 8) | pending;
    if (last != state) {
        fprintf(stderr, "[vr] BotW renderer mode %u, queued %u, game option %u, permission %u\n",
            current, pending, option, vr_labo_option_permitted(0)); last = state;
    }
    if (option) { /* This experiment must not silently use a Labo-option request. */
        static int said; if (!said++) fprintf(stderr, "[vr] game option unexpectedly set; refusing to use it for our renderer\n");
        return;
    }
    if (current != 2 && pending == 0xff) {
        unsigned char requested = 2;
        memcpy((void *)(graphics + mode_offset), &requested, 1);
        fprintf(stderr, "[vr] BotW: queued our renderer mode %u; game option/request untouched\n", requested);
    }
    if (current >= 2 && vr_labo_enabled()) {
        vr_labo_begin();
        vr_views views;
        if(vr_link_views(&views)&&isfinite(views.ipd)&&views.ipd>0&&views.ipd<.2f){
            float *native_half=botw_vr_base?botw_vr_native_half():NULL;
            if(native_half){extern int vr_ipd_pct(void);float half=views.ipd*.5f*vr_ipd_pct()/100.0f;memcpy(native_half,&half,4);}
        }
    }else{
        vr_labo_end();
    }
}

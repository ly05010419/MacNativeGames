#include "vr_labo.h"
#include <TargetConditionals.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

unsigned char vr_labo_option_permitted(unsigned char show_error) {
    (void)show_error;
    return 0;
}

int vr_labo_enabled(void) {
#if defined(GAME_BOTW) && TARGET_OS_OSX
    if (getenv("TOTK_EXPORT_IOS")) return 0;
    const char *control = getenv("TOTK_VR_CONTROL");
    return control && *control;
#else
    return 0;
#endif
}

static _Atomic int active;
int vr_labo_active(void) { return atomic_load(&active)&&vr_labo_enabled(); }
void vr_labo_begin(void) { atomic_store(&active,vr_labo_enabled()); }
void vr_labo_end(void) { atomic_store(&active,0); }

int vr_labo_shader_pair(const char *vertex, const char *fragment) {
    static const char vs[] = "5e9f57168d89a543a537832326b844bcba3255df655c10dc9359cb195304e897";
    static const char fs[] = "bd7ffdd435c6c3bc997653d5fb9a0cc7f2f411dd51ce88c3cafe6574c8624908";
    return vertex && fragment && strlen(vertex) >= 64 && strlen(fragment) >= 64 &&
           !memcmp(vertex, vs, 64) && !memcmp(fragment, fs, 64) &&
           (!vertex[64] || vertex[64] == '-') && (!fragment[64] || fragment[64] == '-');
}
/* A shader's full identity, with or without the translator's suffix. */
static int key_is(const char *have, const char *want) {
    return have && strlen(have) >= 64 && !memcmp(have, want, 64) && (!have[64] || have[64] == '-');
}
/* Each game version ships its own compiled lens, mask and UI programs. The 1.9.0 identities were read from complete
   frames of the world, inventory and map (2026-11-22, run keys01): with only 1.6.0's here, 1.9.0 never captured its eyes
   and the headset showed the whole two-lens window on a flat panel. */
int vr_labo_lens_pair(const char *vertex, const char *fragment) {
    static const char *const pairs[][2] = {
        {"e0467470d532d62384dd46e87619cab73382a02de7d51a1522c0bd23c404245b", "cfa1bb2deb2e2cc1b367027250a3c03c4a48d105ecda5262a96b2c96d708918e"},
        {"4e08f0d81ca7280655a107f48dbba6363b0bdf6c5e07c9280c3447878a69675c", "9c8053675febfbb451eb9a9d6abf807e7f7e43045b663c6fc4b4c0fe381403e6"},   // 1.9.0
    };
    for (unsigned i = 0; i < sizeof pairs / sizeof *pairs; i++)
        if (key_is(vertex, pairs[i][0]) && key_is(fragment, pairs[i][1])) return 1;
    return 0;
}
/* The native comfort mask is a separate draw before the lens LUT. Its exact
   shader pair is omitted because Quest supplies its own display optics. */
int vr_labo_mask_pair(const char *vertex,const char *fragment) {
    static const char vs[]="1f037afbe71a66f468ae9bb31a29e6198278c99957d7cec2642c69c49cb3c13d";
    static const char *const fs[]={
        "eaa948a4d4c418616c67016b2ae92ad64c55dbf060a320710585da88c29e40be",
        "71f5167fe36c717fe88e94dc48a6a0f820ef7f5c797d4241653cb7527424dc87",   // 1.9.0: menus only, the last draw into the canvas
    };
    if(!key_is(vertex,vs))return 0;
    for(unsigned i=0;i<sizeof fs/sizeof *fs;i++)if(key_is(fragment,fs[i]))return 1;
    return 0;
}
/* Original UI vertex programs recorded writing both halves of the native
 * canvas in the inventory and map. Full shader identities, never size alone. */
int vr_native_ui_vertex(const char *vertex) {
    static const char *keys[] = {
        "62f5ad35ffe7b2699c002728041fb83bd2b941b19d1936a4b369eeecab6e879d",
        "35e3c8217a496bdefd3393f2a514ff1d8c54958d28a12c0e7ccaee5445e97132",
        "af24932849a17f4d5d0635e5f090bd65004f8d6eeead7e2ad42d6ff38f3c77ab",
        "0efd7d0b6c368cb1be82333ee021ec0b864b407c3e466fcd0d2ec36ae048a5a9",
        "3bd85812eda98e6b0cd7d9588176769c44f22b30f0bfe4d9b856f76e1524589c",
        "5ecc10804849bfbbdf9f99aef7390db2a4ee83ede9f4cd42f5a7053976380881",
        "46c3223d1857bf0995ce7478539cdad47a170f10ae7c5f51061e9dda83774713",
        /* 1.9.0: every program that writes both halves of the canvas with the widget contract (CB0 560 bytes, CB10 32
           bytes holding the eye's side, not instanced). */
        "4fb3d55c477452767c643bc21cbd1619915cd8b7e9b1ff3f4d7e1a77e4794959",
        "51deaed0b27374b944d36f6520039557b445d249cf8c19c2a11c220e54b8757e",
        "5275c47bd2f93f126fe5d71bb229c48c878c6a24e358848294dd586c78a37cbc",
        "71018be49e22ab4af9a72c2614ebf8fefa671d8aa8cf7076b21f4e0f6b76c935",
        "78a4ace2913b091d3bb0773f6b5757ca176f13b969b227c6f931c8a465995c7a",
        "86208e96540877ab003e352d6430874e039bfc204bee1e962c33e90516568fc9",
        "9f8de48697e031b51142e2c467de02774e41e6b6bcc302434c3dbddbcf3ed5f2",
        "a983fef957911db822215a147b305c1c55bb27963360e71ccf7ce915a1f287b1"
    };
    for (unsigned i=0;i<sizeof keys/sizeof *keys;i++)
        if (key_is(vertex,keys[i])) return 1;
    return vr_native_ui_text_vertex(vertex);
}
/* Instanced glyphs use a precombined P*model in CB0 (128 bytes), but the
 * same canvas/eye UI camera and the same native-eye CB10 as the widgets. */
int vr_native_ui_text_vertex(const char *vertex) {
    static const char *const keys[]={
        "d80c41a52116d05812af0999ebe1f6c4351a750d80d7bb4e33f30df8dc019929",
        "21fe759b8114941c46161cc7f5dad84707fa4c2540674cdc5ea75abf697634ed",   // 1.9.0 (CB0 128 bytes, instanced)
        "60e2041ed375cae126356c554e91a2349e3f82f89986e52ecc9c8d646fb7a396",   // 1.9.0 (CB0 128 bytes, instanced)
    };
    for(unsigned i=0;i<sizeof keys/sizeof *keys;i++)if(key_is(vertex,keys[i]))return 1;
    return 0;
}
int vr_labo_plain_mesh(const char *vertex) {
    /* This complete VS defines the UV/position mapping for several circular
       post passes (deferred composition, depth of field and tone mapping).
       Their fragments stay intact; changing only the last pass leaves a hole. */
    static const char vs[] = "1f037afbe71a66f468ae9bb31a29e6198278c99957d7cec2642c69c49cb3c13d";
    return vertex && strlen(vertex)>=64 && !memcmp(vertex,vs,64) && (!vertex[64]||vertex[64]=='-');
}

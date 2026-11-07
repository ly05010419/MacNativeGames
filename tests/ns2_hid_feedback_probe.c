#include "runtime.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* No controller, settings files or service are accessed by this probe. */
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "%s\n", message); exit(77); }
int runtime_setting_word(const char *name, char *word, size_t size) { (void)name; (void)word; (void)size; return 0; }
void runtime_setting_save(const char *name, const char *word, const char *what) { (void)name; (void)word; (void)what; abort(); }
void runtime_rumble_set(unsigned side, float al, float fl, float ah, float fh) { (void)side; (void)al; (void)fl; (void)ah; (void)fh; abort(); }
int runtime_motion_read(RuntimeMotionState *out, int max) { (void)out; (void)max; abort(); }
void runtime_motion_start(void) { abort(); }

int main(int argc, char **argv) {
    const char *pub = "_ZN2nn3hid29IsOunceVibrationDeviceMountedERKNS0_21VibrationDeviceHandleE";
    const char *detail = "_ZN2nn3hid6detail29IsOunceVibrationDeviceMountedEPbRKNS0_21VibrationDeviceHandleE";
    setenv("TOTK_GYRO", "0", 1);
    setenv("TOTK_SLOT_OWNER", "codex", 1);
    for (int on = 0; on <= 1; on++) {
        setenv("TOTK_RUMBLE", on ? "1" : "0", 1);
        bool (*mounted)(const uint32_t *) = runtime_hid_feedback_override(pub);
        uint32_t (*mounted_detail)(bool *, const uint32_t *) = runtime_hid_feedback_override(detail);
#ifdef GAME_NS2
        assert(mounted && mounted_detail);
        if (argc > 1 && !strcmp(argv[1], "null-output")) { mounted_detail(NULL, NULL); abort(); }
        if (on && argc > 1 && !strcmp(argv[1], "null-handle")) { bool b; mounted_detail(&b, NULL); abort(); }
        assert(!mounted(NULL));
        const uint32_t handles[] = {3, 0x10003, 4, 0x10004, 0x20003, 0, 5};
        for (unsigned i = 0; i < sizeof handles / sizeof *handles; i++) {
            bool expected = on && i < 4;
            struct { unsigned char before; bool value; unsigned char after; } out = {0x5a, !expected, 0xa5};
            assert(mounted(&handles[i]) == expected);
            assert(mounted_detail(&out.value, &handles[i]) == 0);
            assert(out.value == expected && out.before == 0x5a && out.after == 0xa5);
        }
        if (!on) { bool value = true; assert(mounted_detail(&value, NULL) == 0 && !value); }
#else
        (void)argc; (void)argv;
        assert(!mounted && !mounted_detail);
#endif
        /* The existing Switch 1 query keeps its lookup behavior. */
        void *old = runtime_hid_feedback_override("_ZN2nn3hid6detail24IsVibrationDeviceMountedEPbRKNS0_21VibrationDeviceHandleE");
        assert(!!old == !!on);
        assert(!runtime_hid_feedback_override("unknown-hid-symbol"));
    }
    puts("pass: NS2 vibration mounted query ABI and routing");
    return 0;
}

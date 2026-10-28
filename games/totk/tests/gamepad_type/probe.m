/* runtime/gamepad.m's controller type (key 6): the cycle, the word it keeps in settings.json (the one file every setting
   is in) and reading it back at the next start. The real file is compiled in; no controller is needed. */
#include "../../runtime/gamepad.m"

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
uint64_t runtime_now_ns(void) { return 0; }
void runtime_feedback_attach(GCController *controller) { (void)controller; }
void runtime_feedback_detach(GCController *controller) { (void)controller; }
void runtime_feedback_quiet(void) {}
int runtime_gyro_enabled(void) { return 0; }
void runtime_motion_mouse(int on) { (void)on; }
void runtime_motion_feed_mouse(const double acc[3], const double gyro[3], uint64_t now) { (void)acc; (void)gyro; (void)now; }

static int failures, checked;
static void check(int ok, const char *what) { checked++; if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", what); } }
static void write_text(const char *path, const char *text) { FILE *f = fopen(path, "w"); fputs(text, f); fclose(f); }
static int word_is(const char *path, const char *key, const char *word) {
    char got[2048] = {0}, want[80];
    FILE *f = fopen(path, "r"); if (!f) return 0;
    size_t n = fread(got, 1, sizeof got - 1, f); fclose(f); got[n] = 0;
    snprintf(want, sizeof want, "\"%s\" : \"%s\"", key, word);
    return strstr(got, want) != NULL;
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) { fprintf(stderr, "usage: probe <empty folder>\n"); return 2; }
    char controls[1024], saved[1100];
    snprintf(controls, sizeof controls, "%s/controls.txt", argv[1]);
    snprintf(saved, sizeof saved, "%s/settings.json", argv[1]);

    unsetenv("TOTK_CONTROLS");
    int detected = 7;
    check(runtime_gamepad_type(&detected) == GAMEPAD_TYPE_AUTO && detected == -1, "starts at auto; no controller: detected -1");
    check(runtime_gamepad_type_set(GAMEPAD_TYPE_XBOX) == GAMEPAD_TYPE_XBOX && access(saved, F_OK) != 0, "without TOTK_CONTROLS nothing is saved");

    setenv("TOTK_CONTROLS", controls, 1);
    atomic_store(&controllerType, GAMEPAD_TYPE_AUTO);
    check(runtime_gamepad_type_set(GAMEPAD_TYPE_XBOX) == GAMEPAD_TYPE_XBOX && word_is(saved, "controller-type", "xbox"), "settings: Xbox, saved");
    check(runtime_gamepad_type_set(GAMEPAD_TYPE_PLAYSTATION) == GAMEPAD_TYPE_PLAYSTATION && word_is(saved, "controller-type", "playstation"), "settings: PlayStation, saved");
    check(runtime_gamepad_type_set(GAMEPAD_TYPE_SWITCH) == GAMEPAD_TYPE_SWITCH && word_is(saved, "controller-type", "switch"), "settings: Switch, saved");
    check(runtime_gamepad_type_set(GAMEPAD_TYPE_COUNT) == GAMEPAD_TYPE_SWITCH && runtime_gamepad_type_set(-1) == GAMEPAD_TYPE_SWITCH, "not a type: unchanged");
    check(access(saved, F_OK) == 0 && runtime_gamepad_type(NULL) == GAMEPAD_TYPE_SWITCH, "the type holds");

    atomic_store(&controllerType, GAMEPAD_TYPE_AUTO); load_controller_type();
    check(runtime_gamepad_type(NULL) == GAMEPAD_TYPE_SWITCH, "the next start reads the saved type");
    check(runtime_gamepad_type_set(GAMEPAD_TYPE_AUTO) == GAMEPAD_TYPE_AUTO && word_is(saved, "controller-type", "auto"), "settings back to auto, saved");

    write_text(saved, "{\"controller-type\":\"dualsense\"}");
    settingsWords = nil;   /* the store reads once a session; a hand-edited file is a new session */
    atomic_store(&controllerType, GAMEPAD_TYPE_AUTO); load_controller_type();
    check(runtime_gamepad_type(NULL) == GAMEPAD_TYPE_AUTO, "a word that is not a type: auto");
    unlink(saved); settingsWords = nil; load_controller_type();
    check(runtime_gamepad_type(NULL) == GAMEPAD_TYPE_AUTO, "no settings.json: auto");

    setenv("TOTK_CONTROLS", "/nonexistent-folder-for-probe/controls.txt", 1);
    check(runtime_gamepad_type_set(GAMEPAD_TYPE_XBOX) == GAMEPAD_TYPE_XBOX, "a folder that cannot be written: the type still changes for this run");

    printf("{\"status\":\"%s\",\"probe\":\"gamepad-type\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checked, failures);
    return failures != 0;
}}

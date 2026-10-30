/* runtime/gamepad.m's controller type (key 6): the cycle, the saved controller-type.txt next to controls.txt and reading it
   back at the next start. The real file is compiled in; no controller is needed (GameController lists none). */
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
static int file_says(const char *path, const char *text) {
    char got[64] = {0}; FILE *f = fopen(path, "r"); if (!f) return 0;
    size_t n = fread(got, 1, sizeof got - 1, f); fclose(f); got[n] = 0; return !strcmp(got, text);
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) { fprintf(stderr, "usage: probe <empty folder>\n"); return 2; }
    char controls[1024], saved[1100];
    snprintf(controls, sizeof controls, "%s/controls.txt", argv[1]);
    snprintf(saved, sizeof saved, "%s/controller-type.txt", argv[1]);

    unsetenv("TOTK_CONTROLS");
    int detected = 7;
    check(runtime_gamepad_type(&detected) == GAMEPAD_TYPE_AUTO && detected == -1, "starts at auto; no controller: detected -1");
    check(runtime_gamepad_type_cycle() == GAMEPAD_TYPE_XBOX && access(saved, F_OK) != 0, "without TOTK_CONTROLS nothing is saved");

    setenv("TOTK_CONTROLS", controls, 1);
    atomic_store(&controllerType, GAMEPAD_TYPE_AUTO);
    check(runtime_gamepad_type_cycle() == GAMEPAD_TYPE_XBOX && file_says(saved, "xbox\n"), "key 6: Xbox, saved");
    check(runtime_gamepad_type_cycle() == GAMEPAD_TYPE_PLAYSTATION && file_says(saved, "playstation\n"), "key 6: PlayStation, saved");
    check(runtime_gamepad_type_cycle() == GAMEPAD_TYPE_SWITCH && file_says(saved, "switch\n"), "key 6: Switch, saved");
    check(access(saved, F_OK) == 0 && runtime_gamepad_type(NULL) == GAMEPAD_TYPE_SWITCH, "the type holds");

    atomic_store(&controllerType, GAMEPAD_TYPE_AUTO); load_controller_type();
    check(runtime_gamepad_type(NULL) == GAMEPAD_TYPE_SWITCH, "the next start reads the saved type");
    check(runtime_gamepad_type_cycle() == GAMEPAD_TYPE_AUTO && file_says(saved, "auto\n"), "key 6 back to auto, saved");

    write_text(saved, "dualsense\n");
    atomic_store(&controllerType, GAMEPAD_TYPE_AUTO); load_controller_type();
    check(runtime_gamepad_type(NULL) == GAMEPAD_TYPE_AUTO, "a word that is not a type: auto");
    unlink(saved); load_controller_type();
    check(runtime_gamepad_type(NULL) == GAMEPAD_TYPE_AUTO, "no file: auto");

    setenv("TOTK_CONTROLS", "/nonexistent-folder-for-probe/controls.txt", 1);
    check(runtime_gamepad_type_cycle() == GAMEPAD_TYPE_XBOX, "a folder that cannot be written: the type still changes for this run");

    printf("{\"status\":\"%s\",\"probe\":\"gamepad-type\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checked, failures);
    return failures != 0;
}}

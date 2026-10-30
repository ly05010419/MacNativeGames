/* Test the screenshot's folder and file name (games/totk/runtime/screenshot.h): TOTK_SCREENSHOT_DIR, a leading ~, the
   Desktop without it, the game's name and the local time in the name, and (2), (3) ... when that second is taken. The
   PNG write and the window's note need AppKit and a picture, so they are not part of this.
   The environment checks run in a child process (screenshot_child): setenv may reuse the storage of the string it
   replaces, which in the parent would be the caller's own saved value. */
#include "../games/totk/runtime/screenshot.h"
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static void expect(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; } }

/* setenv with a copy of the string: setenv keeps no copy, and the caller's string is one literal the call replaces */
static void set_dir(const char *value) {
    char copy[PATH_MAX];
    snprintf(copy, sizeof copy, "%s", value);
    setenv("TOTK_SCREENSHOT_DIR", copy, 1);
    snprintf(copy, sizeof copy, "%s", "a later call must not change the setting");
}

static void folder_checks(void) {
    char out[PATH_MAX], want[PATH_MAX];
    /* The setting lives in the working folder: a folder and a home of their own, so the checks never touch the real ones. */
    char work[] = "/tmp/totk-shot-setting-XXXXXX";
    expect(mkdtemp(work) != NULL && chdir(work) == 0, "a working folder for the setting");
    char home[] = "/tmp/totk-shot-home-XXXXXX";
    expect(mkdtemp(home) != NULL, "and a home folder beside it");
    setenv("HOME", home, 1);
    setenv("TOTK_SCREENSHOT_DIR", "", 1);
    const char *why = NULL;
    int problem = SHOT_OK;
    /* the panel's row: the folder is kept, made when it is missing, and read back */
    expect(screenshot_directory_set("Shots", &why, &problem, NULL) == 1, "a relative folder kept: made in the home folder");
    snprintf(want, sizeof want, "%s/Shots", home);
    expect(!strcmp(screenshot_directory(out, sizeof out), want), "and read back from there");
    expect(screenshot_directory_set("~/Pictures/Game", &why, &problem, NULL) == 1, "~ for the home folder, its folders made too");
    snprintf(want, sizeof want, "%s/Pictures/Game", home);
    expect(!strcmp(screenshot_directory(out, sizeof out), want), "read back expanded");
    expect(screenshot_directory_set("/tmp", &why, &problem, NULL) == 1, "an absolute folder");
    expect(!strcmp(screenshot_directory(out, sizeof out), "/tmp"), "read back as typed");
    expect(screenshot_directory_set("", &why, &problem, NULL) == 1, "emptied: the Desktop");
    snprintf(want, sizeof want, "%s/Desktop", home);
    expect(!strcmp(screenshot_directory(out, sizeof out), want), "back to the Desktop");
    char file[] = "/tmp/totk-shot-file-XXXXXX";
    int fd = mkstemp(file);
    if (fd >= 0) close(fd);
    expect(screenshot_directory_set(file, &why, &problem, NULL) == 0 && why != NULL, "a file of that name: refused, and why");

    /* the folder in force: the environment first, then the panel's file, then the Desktop; a leading ~ is the home folder */
    setenv("TOTK_SCREENSHOT_DIR", "/tmp/shots-from-the-environment", 1);
    expect(!strcmp(screenshot_folder(out, sizeof out), "/tmp/shots-from-the-environment"), "the environment wins");
    setenv("TOTK_SCREENSHOT_DIR", "/tmp/totk-shot-elsewhere", 1);
    expect(!strcmp(screenshot_folder(out, sizeof out), "/tmp/totk-shot-elsewhere"), "an absolute setting");
    /* Through the environment: a folder the player picks replaces a launch's TOTK_SCREENSHOT_DIR at once, so the choice
       is not hidden until the next start. */
    setenv("TOTK_SCREENSHOT_DIR", "", 1);
    expect(screenshot_directory_set("/tmp/totk-shot-chosen", &why, &problem, NULL) == 1, "the player picks another folder");
    expect(!strcmp(screenshot_folder(out, sizeof out), "/tmp/totk-shot-chosen"), "and it takes over this launch at once");
    setenv("TOTK_SCREENSHOT_DIR", "", 1);
    expect(!strcmp(screenshot_folder(out, sizeof out), "/tmp/totk-shot-chosen"), "with nothing in the environment: the file's folder");
    expect(screenshot_directory_set("", &why, &problem, NULL) == 1, "the player clears it");
    snprintf(want, sizeof want, "%s/Desktop", home);
    expect(!strcmp(screenshot_folder(out, sizeof out), want), "back to the Desktop");
    setenv("TOTK_SCREENSHOT_DIR", "", 1);   /* no setting at all, now that it is cleared */
    expect(!strcmp(screenshot_folder(out, sizeof out), want), "no setting: the Desktop");
    expect(screenshot_directory_set("~", &why, &problem, NULL) == 1, "and the home folder itself is a folder");
    expect(!strcmp(screenshot_directory(out, sizeof out), home), "read back as the home folder");
    set_dir("/tmp/shots");
    expect(!strcmp(screenshot_folder(out, sizeof out), "/tmp/shots"), "a folder of its own");
    set_dir("~/Pictures/Game");
    snprintf(want, sizeof want, "%s/Pictures/Game", home);
    expect(!strcmp(screenshot_folder(out, sizeof out), want), "~ is the home folder");
    set_dir("~");
    expect(!strcmp(screenshot_folder(out, sizeof out), home), "~ alone is the home folder");
    set_dir("~shots");
    snprintf(want, sizeof want, "%s/~shots", home);
    expect(!strcmp(screenshot_folder(out, sizeof out), want), "~ in front of a name is no home folder");
    set_dir("Shots");
    snprintf(want, sizeof want, "%s/Shots", home);
    expect(!strcmp(screenshot_folder(out, sizeof out), want), "a relative folder is from the home folder");
    setenv("TOTK_SCREENSHOT_DIR", "", 1);
    expect(screenshot_directory_set("", &why, &problem, NULL) == 1, "emptied again");
    snprintf(want, sizeof want, "%s/Desktop", home);
    expect(!strcmp(screenshot_folder(out, sizeof out), want), "no setting: the Desktop");
    set_dir("");
    expect(!strcmp(screenshot_folder(out, sizeof out), want), "an empty setting: the Desktop too");
    setenv("HOME", "", 1);
    set_dir("");
    expect(!strcmp(screenshot_folder(out, sizeof out), "./Desktop"), "no HOME: the Desktop of the working folder");
}

/* Why a picture cannot be written: each way told apart, so the note can say which folder and what is wrong with it. */
static void problem_checks(void) {
    int problem = SHOT_OK;
    uint64_t freeBytes = 0;
    const char *why = NULL;
    char folder[] = "/tmp/totk-shot-problem-XXXXXX";
    expect(mkdtemp(folder) != NULL, "a folder for the problem checks");
    expect(screenshot_problem(folder, "x.png", &problem, &freeBytes) == 1 && problem == SHOT_OK, "a writable folder: fine");
    expect(freeBytes > 0, "and its free space is read");

    /* a folder that went away: an external disk unplugged, or the folder deleted in Finder */
    char gone[PATH_MAX];
    snprintf(gone, sizeof gone, "%s/gone", folder);
    expect(screenshot_problem(gone, "x.png", &problem, NULL) == 0 && problem == SHOT_NO_FOLDER, "a folder that is not there");
    expect(strlen(screenshot_problem_text(SHOT_NO_FOLDER)) > 3, "and a reason that says so");

    /* a file at the path, which the chooser cannot produce but a script can */
    char file[PATH_MAX];
    snprintf(file, sizeof file, "%s/notes.txt", folder);
    FILE *created = fopen(file, "w");
    expect(created != NULL, "a file beside it");
    if (created) fclose(created);
    expect(screenshot_problem(file, "x.png", &problem, NULL) == 0 && problem == SHOT_NOT_FOLDER, "a file at the path: not a folder");

    /* no permission to write: one this account owns with the write bit off. Not a chmod of anything outside the test: it
       makes its own folder and asks, which also holds where the test runs as root (the bits are then ignored). */
    char closed[PATH_MAX];
    snprintf(closed, sizeof closed, "%s/closed", folder);
    expect(mkdir(closed, 0755) == 0, "a folder it will close");
    if (chmod(closed, 0555) == 0) {
        int refused = screenshot_problem(closed, "x.png", &problem, NULL) == 0;
        if (geteuid() == 0) expect(!refused || problem == SHOT_NO_PERMISSION, "root may write anything");
        else expect(refused && problem == SHOT_NO_PERMISSION, "a folder without the write bit: no permission");
        if (geteuid() != 0) expect(screenshot_directory_set(closed, &why, &problem, NULL) == 0 && problem == SHOT_NO_PERMISSION,
                                  "and the chooser refuses it, saying why");
        chmod(closed, 0755);
    }
    expect(screenshot_problem(folder, NULL, &problem, NULL) == 1, "the folder alone can be judged: fine");

    /* a path that cannot fit */
    char longPath[PATH_MAX];
    memset(longPath, 'a', sizeof longPath);
    longPath[0] = '/';
    longPath[sizeof longPath - 1] = 0;
    expect(screenshot_problem(longPath, "x.png", &problem, NULL) == 0 && problem == SHOT_PATH_LONG, "an impossible path");
    char longName[PATH_MAX];
    memset(longName, 'b', sizeof longName - 1);
    longName[sizeof longName - 1] = 0;
    snprintf(longPath, 200, "%s", folder);
    expect(screenshot_problem(longPath, longName, &problem, NULL) == 0 && problem == SHOT_PATH_LONG, "a name that cannot fit");

    /* every reason is short and never empty where one is needed; the phone's two are among them */
    int all[] = {SHOT_NO_FOLDER, SHOT_NOT_FOLDER, SHOT_NO_PERMISSION, SHOT_PATH_LONG, SHOT_FULL, SHOT_NO_PHOTOS, SHOT_TOO_LARGE};
    for (unsigned i = 0; i < sizeof all / sizeof *all; i++) {
        const char *text = screenshot_problem_text(all[i]);
        expect(text && strlen(text) > 3 && strlen(text) < 80, "each reason is one short line");
        for (int k = 0; k < (int)i; k++) expect(strcmp(screenshot_problem_text(all[k]), text) != 0, "and every one says something else");
    }
    expect(!strcmp(screenshot_problem_text(SHOT_OK), ""), "and nothing is said when nothing is wrong");

    /* the photo library's album, where a phone's pictures go */
    expect(strlen(TOTK_SCREENSHOT_ALBUM) > 0 && strlen(TOTK_SCREENSHOT_ALBUM) < 64, "the album has a name the library accepts");
}

/* The file name: the game, the date and the local time, (2) / (3) ... within one second, a short buffer refused. */
static void name_checks(void) {
    char folder[] = "/tmp/totk-screenshot-test-XXXXXX";
    char path[PATH_MAX];
    expect(mkdtemp(folder) != NULL, "a temporary folder");
    struct tm at;
    memset(&at, 0, sizeof at);
    at.tm_year = 2026 - 1900; at.tm_mon = 10 - 1; at.tm_mday = 30; at.tm_hour = 14; at.tm_min = 5; at.tm_sec = 32;
    expect(screenshot_path(folder, "Tears of the Kingdom", &at, path, sizeof path) == path, "the first name");
    expect(strstr(path, "/Tears of the Kingdom 2026-10-30 at 14.05.32.png") != NULL, "the game, the date, the time");
    char first[PATH_MAX];
    snprintf(first, sizeof first, "%s", path);
    FILE *file = fopen(first, "w");
    expect(file != NULL, "a file of that name");
    if (file) fclose(file);
    expect(screenshot_path(folder, "Tears of the Kingdom", &at, path, sizeof path) == path, "the second name");
    expect(strstr(path, " at 14.05.32 (2).png") != NULL, "a taken second counts on: (2)");
    for (int n = 2; n <= 4; n++) {
        file = fopen(path, "w");
        if (file) fclose(file);
        expect(screenshot_path(folder, "Tears of the Kingdom", &at, path, sizeof path) == path, "a name again");
    }
    expect(screenshot_path(folder, "Tears of the Kingdom", &at, path, sizeof path) == path, "the fifth name");
    expect(strstr(path, " at 14.05.32 (5).png") != NULL, "and (5)");
    at.tm_sec = 33;
    expect(strstr(screenshot_path(folder, "Tears of the Kingdom", &at, path, sizeof path), " at 14.05.33.png") != NULL, "the next second starts over");
    at.tm_mday = 1; at.tm_hour = 0; at.tm_min = 0; at.tm_sec = 0;
    expect(strstr(screenshot_path(folder, "Tears of the Kingdom", &at, path, sizeof path), "2026-10-01 at 00.00.00.png") != NULL, "padded, midnight");
    at.tm_mon = 12 - 1; at.tm_mday = 31; at.tm_hour = 23; at.tm_min = 59; at.tm_sec = 59;
    expect(strstr(screenshot_path(folder, "Breath of the Wild", &at, path, sizeof path), "Breath of the Wild 2026-12-31 at 23.59.59.png") != NULL,
           "both games' names fit");
    char tiny[24];
    expect(screenshot_path(folder, "Tears of the Kingdom", &at, tiny, sizeof tiny) == NULL, "a buffer too small: NULL");
    expect(screenshot_path(folder, "x", &at, path, 10) == NULL, "and so is one with no room at all");
    /* every name of one second taken: NULL, so the writer says so instead of overwriting */
    struct tm busy = at;
    busy.tm_sec = 7;
    char filled[PATH_MAX];
    unsigned made = 0;
    for (; made < 1000; made++) {
        if (!screenshot_path(folder, "busy", &busy, filled, sizeof filled)) break;
        file = fopen(filled, "w");
        if (file) fclose(file);
    }
    expect(made == 1000, "a thousand names, then none");
    expect(screenshot_path(folder, "busy", &busy, path, sizeof path) == NULL, "a thousand names of one second: NULL");
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "folder")) {   /* the child: the environment checks, in folders of its own */
        folder_checks();
        return failures ? 1 : 0;
    }
    pid_t child = fork();
    if (child == 0) {
        folder_checks();
        _exit(failures ? 1 : 0);
    }
    if (child < 0) { fprintf(stderr, "FAIL: fork for the environment checks\n"); failures++; }
    else {
        int status = 0;
        waitpid(child, &status, 0);
        expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the folder checks in a child process");
    }
    problem_checks();
    name_checks();
    if (failures) { fprintf(stderr, "%d failed\n", failures); return 1; }
    puts("screenshot: all checks pass");
    return 0;
}

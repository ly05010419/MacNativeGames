/* The local users store (runtime/account.c): a fresh profile takes the user "Local", a profile from before the
   users keeps its identity, users are added and chosen, names are cleaned, the store survives a restart (each
   scenario runs in a forked child, since initialize happens once per process, and the parent only reads the files)
   and a corrupt users.txt is recovered (the binary re-execs itself). No game, window or guest. */
#include "../games/totk/runtime/users.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "runtime_fail: %s\n", message); exit(1); }

void runtime_account_uid(uint64_t out[2]);      /* runtime/fs_host.h: the user this run opened with */
void *runtime_account_override(const char *name);   /* runtime/runtime.h: the nn::account entries by mangled name */

static char dir[256];

static void read_file(const char *name, char *out, size_t cap) {
    char path[512]; snprintf(path, sizeof path, "%s/%s", dir, name);
    int fd = open(path, O_RDONLY); assert(fd >= 0);
    ssize_t n = read(fd, out, cap - 1); assert(n >= 0); out[n] = 0; close(fd);
}
static void write_file(const char *name, const void *data, size_t n) {
    char path[512]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb"); assert(f); assert(fwrite(data, 1, n, f) == n); assert(!fclose(f));
}
/* The line that carries the '*' (the user the next launch opens with), or -1. */
static int marked_line(const char *text) {
    int marked = -1, line = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '*' && marked < 0) marked = line;
        if (*p == '\n') line++;
    }
    return marked;
}
/* scenario: the body runs in a forked child with TOTK_PROFILE_DIR set (a child that fails stops loudly); the
   parent waits for it, so its own process stays uninitialized and reads only the files the child left */
static void new_dir(void) { snprintf(dir, sizeof dir, "/tmp/mng-users-XXXXXX"); assert(mkdtemp(dir)); }
static void scenario(void (*body)(void)) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) { setenv("TOTK_PROFILE_DIR", dir, 1); body(); _exit(0); }
    int status; assert(waitpid(pid, &status, 0) == pid && status == 0);
}

static void fresh_profile(void) {
    RuntimeUser u[RUNTIME_USERS_MAX];
    assert(runtime_users_get(u, RUNTIME_USERS_MAX) == 1 && !strcmp(u[0].name, "Local"));
    assert(u[0].lo || u[0].hi);
    RuntimeUser me; assert(!runtime_users_active(&me) && me.lo == u[0].lo && me.hi == u[0].hi);
    struct stat st;
    char path[512]; snprintf(path, sizeof path, "%s/account.uid", dir);
    assert(stat(path, &st) != 0);   /* a fresh profile does not write the file of the single-user past */
    snprintf(path, sizeof path, "%s/users.txt", dir);
    assert(stat(path, &st) == 0 && S_ISREG(st.st_mode));
}
static void legacy_profile(void) {
    RuntimeUser u[RUNTIME_USERS_MAX];
    assert(runtime_users_get(u, RUNTIME_USERS_MAX) == 1 && !strcmp(u[0].name, "Local"));
    assert(u[0].lo == 0x0102030405060708ull && u[0].hi == 0x1112131415161718ull);   /* the old identity, unchanged */
    struct stat st; char path[512]; snprintf(path, sizeof path, "%s/account.uid", dir);
    assert(stat(path, &st) == 0 && st.st_size == 16);   /* kept, so an older build still runs */
}
static void mutations(void) {
    RuntimeUser u[RUNTIME_USERS_MAX];
    assert(runtime_users_get(u, RUNTIME_USERS_MAX) == 1);   /* the fresh "Local" */
    assert(runtime_users_add(NULL) == -1 && runtime_users_add("") == -1 && runtime_users_add("   ") == -1);
    assert(runtime_users_add("  Zelda  ") == 1);
    assert(runtime_users_add("a\tb\nc") == 2);              /* control characters dropped */
    assert(runtime_users_add("一二三四五六七八九十") == 3);
    assert(runtime_users_add("一二三四五六七八九十十一") == 4);   /* cut at 31 bytes, whole characters only */
    assert(runtime_users_add("Zelda") == 5);                /* a name may repeat */
    assert(runtime_users_get(u, RUNTIME_USERS_MAX) == 6);
    assert(!strcmp(u[1].name, "Zelda") && !strcmp(u[2].name, "abc") && strlen(u[3].name) == 30 && strlen(u[4].name) == 30);
    uint64_t me[2]; runtime_account_uid(me);
    assert(me[0] == u[0].lo && me[1] == u[0].hi);           /* this run keeps its user */
    assert(!runtime_users_choose(2) && !runtime_users_choose(2));
    runtime_account_uid(me); assert(me[0] == u[0].lo && me[1] == u[0].hi);   /* still, mid-run */
    RuntimeUser next; assert(!runtime_users_active(&next) && next.lo == u[2].lo && next.hi == u[2].hi);
    assert(runtime_users_choose(-1) == -1 && runtime_users_choose(6) == -1);
    assert(runtime_users_add("七八") == 6 && runtime_users_add("九十") == 7);   /* 8, as on a console */
    assert(runtime_users_add("one too many") == -1);
    assert(runtime_users_get(u, RUNTIME_USERS_MAX) == 8);
    /* the game's view: the name and existence of every user, and nobody else's */
    uint32_t (*nick)(char *, const uint64_t *) = runtime_account_override("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE");
    uint32_t (*exists)(unsigned char *, const uint64_t *) = runtime_account_override("_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE");
    assert(nick && exists);
    char name[32];
    for (int i = 0; i < 8; i++) {
        uint64_t id[2] = {u[i].lo, u[i].hi}; unsigned char here = 0;
        assert(exists(&here, id) == 0 && here == 1);
        assert(nick(name, id) == 0 && !strcmp(name, u[i].name));
    }
    uint64_t nobody[2] = {0xdeadbeefdeadbeefull, 0}; unsigned char here = 1;
    assert(exists(&here, nobody) == 0 && here == 0 && nick(name, nobody) == 0x7c67);
}
static void restart(void) {
    RuntimeUser u[RUNTIME_USERS_MAX];
    assert(runtime_users_get(u, RUNTIME_USERS_MAX) == 8);
    assert(!strcmp(u[2].name, "abc") && strlen(u[3].name) == 30 && !strcmp(u[5].name, "Zelda"));
    uint64_t me[2]; runtime_account_uid(me);
    assert(me[0] == u[2].lo && me[1] == u[2].hi);   /* the pick of the run before */
}
static void marked_read(void) {
    RuntimeUser u[RUNTIME_USERS_MAX];
    assert(runtime_users_get(u, RUNTIME_USERS_MAX) == 2);
    assert(!strcmp(u[0].name, "Local") && !strcmp(u[1].name, "Zelda"));
    uint64_t me[2]; runtime_account_uid(me);
    assert(me[0] == 1 && me[1] == 0xaaaabbbbccccddddull);   /* the marked user */
}
static void garbage(void) {
    RuntimeUser u[RUNTIME_USERS_MAX];
    assert(runtime_users_get(u, RUNTIME_USERS_MAX) == 1 && !strcmp(u[0].name, "Local"));
    assert(u[0].lo || u[0].hi);   /* a store where nothing parses starts over */
}

int main(int argc, char **argv) {
    /* recovery phase, re-execed with a corrupt users.txt: the valid lines survive in order, the marked one is in
       force, the garbage is skipped */
    if (argc == 3 && !strcmp(argv[1], "recover")) {
        snprintf(dir, sizeof dir, "%s", argv[2]);
        RuntimeUser u[RUNTIME_USERS_MAX];
        assert(runtime_users_get(u, RUNTIME_USERS_MAX) == 2);
        assert(!strcmp(u[0].name, "Local") && !strcmp(u[1].name, "Zelda"));
        uint64_t me[2]; runtime_account_uid(me);
        assert(me[0] == u[1].lo && me[1] == u[1].hi);
        return 0;
    }
    assert(argc == 1);

    new_dir();
    scenario(fresh_profile);
    char text[4096]; read_file("users.txt", text, sizeof text);
    assert(marked_line(text) == 0 && strlen(text) == 1 + 32 + 1 + 5 + 1);   /* *<32 hex>\tLocal\n */

    new_dir();
    const uint64_t legacy[2] = {0x0102030405060708ull, 0x1112131415161718ull};
    write_file("account.uid", legacy, sizeof legacy);
    scenario(legacy_profile);

    new_dir();
    scenario(mutations);
    read_file("users.txt", text, sizeof text);
    {   /* written the way the page reads it: the chosen user marked, one line per user */
        int lines = 0; for (const char *p = text; *p; p++) lines += (*p == '\n');
        assert(lines == 8 && marked_line(text) == 2);
    }

    scenario(restart);   /* the same store, read by a process of its own: the pick of the run before */
    read_file("users.txt", text, sizeof text);
    assert(marked_line(text) == 2);   /* unchanged: nothing rewrote it */

    new_dir();   /* a handcrafted store: the marked user is the one in force */
    write_file("users.txt", "01020304050607081112131415161718\tLocal\n"
                            "*aaaabbbbccccdddd0000000000000001\tZelda\n", 79);
    scenario(marked_read);
    read_file("users.txt", text, sizeof text);
    assert(marked_line(text) == 1);   /* unchanged: nothing rewrote it */

    new_dir();   /* the corrupt file, recovered in a process of its own */
    const char *corrupt =
        "garbage line without a tab\n"
        "0x01020304050607081112131415161718\tLocal\n"       /* not hex */
        "01020304050607081112131415161718 no tab\n"         /* no tab */
        "01020304050607081112131415161718\tLocal\n"
        "*aaaabbbbccccdddd0000000000000001\tZelda\n"
        "*01020304050607081112131415161718\tduplicate\n"    /* the id twice */
        "*this line's mark is consumed by nothing\n";       /* a mark on a line that never parses */
    write_file("users.txt", corrupt, strlen(corrupt));
    pid_t recovery = fork();
    assert(recovery >= 0);
    if (recovery == 0) { setenv("TOTK_PROFILE_DIR", dir, 1); execl(argv[0], argv[0], "recover", dir, (char *)NULL); _exit(127); }
    int status; assert(waitpid(recovery, &status, 0) == recovery && status == 0);

    new_dir();
    write_file("users.txt", "nothing here\n", 13);
    scenario(garbage);

    puts("pass: users (fresh profile, legacy identity, add / choose and the running user, names cleaned, 8 the most, the game's view, restart, corrupt file, unparsable file)");
    return 0;
}

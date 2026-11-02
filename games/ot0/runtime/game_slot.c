/* Game slot: at most one game (or GPU probe) runs at a time on this Mac.
   One game takes about 6.6 GB of the 16 GB; two need about 13 GB, and exhausted memory is what made the GPU driver
   panic the kernel four times on 2026-10-12. Two games would also share one GPU, so neither one's frame times mean
   anything. Several agents (Claude, Codex) and the user's own desktop launcher start games, so the rule is enforced
   here, in every `boot` and `probe-gpu-*` run, rather than in each script.

   The holder keeps an exclusive flock(2) on <slot dir>/lock for the life of its game; the kernel drops the lock when
   the process exits or crashes, so a dead holder never blocks anyone. Waiters line up first come first served. A game
   started for the user (TOTK_SLOT_OWNER=user, the desktop launcher) goes ahead of the queue and stops an agent's game
   that holds the slot: it first records the preemption, then sends SIGTERM, so the agent's script knows to rerun that
   test once the slot is free again. A wrapper that already holds the lock (scripts/dev/game_slot.py) passes it down
   as TOTK_GAME_SLOT_FD. Files in the slot directory (shared with scripts/dev/game_slot.py; change both together):
     lock               flock(2) target
     holder             key=value lines about the current holder; stale once the lock is free
     queue/<ns>-<pid>   waiters (key=value: pid, start, owner, purpose, since)
     user-request       the user's game while it waits for the slot
     preempted/<token>  written just before a holder is stopped for the user
     history.log        one line per acquire / start / preempt / exit
   The slot directory is TOTK_GAME_SLOT_DIR, else analysis/game-slot next to this binary's build/ directory; every
   worktree links analysis/ to the main checkout (scripts/dev/new_worktree.sh), so all checkouts share one slot. */
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include "runtime.h"
/* One app per phone: the slot only exists to keep several games off one Mac. */
void runtime_game_slot_acquire(const char *mode) { (void)mode; }
#else
#include "runtime.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char slot_dir[PATH_MAX], lock_path[PATH_MAX], my_queue[PATH_MAX];
static char owner[33], purpose[200], token[48], mode_name[64];
static int lock_fd = -1, acquired;
static time_t acquired_at;

_Noreturn static void slot_fail(const char *what, const char *path) {
    fprintf(stderr, "[game-slot] %s %s: %s\n", what, path ? path : "", strerror(errno));
    runtime_fail("game slot: see the [game-slot] line above");
}

static void path_in(char *out, const char *name) {
    if (snprintf(out, PATH_MAX, "%s/%s", slot_dir, name) >= PATH_MAX) runtime_fail("game slot path too long");
}

static void clean_copy(char *dst, size_t n, const char *src) {   /* one line, no control characters */
    size_t i = 0;
    for (; src && src[i] && i + 1 < n; i++) dst[i] = (unsigned char)src[i] < 0x20 ? ' ' : src[i];
    dst[i] = 0;
}

static int kv_get(const char *path, const char *key, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024]; size_t kl = strlen(key); int found = 0;
    while (!found && fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            char *v = line + kl + 1; v[strcspn(v, "\n")] = 0;
            snprintf(out, n, "%s", v); found = 1;
        }
    fclose(f);
    return found;
}

static long long kv_num(const char *path, const char *key) {
    char b[32];
    return kv_get(path, key, b, sizeof b) ? atoll(b) : 0;
}

static void write_atomic(const char *path, const char *text) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, getpid()) >= (int)sizeof tmp) runtime_fail("game slot path too long");
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) slot_fail("cannot write", tmp);
    size_t len = strlen(text);
    if (write(fd, text, len) != (ssize_t)len) slot_fail("cannot write", tmp);
    close(fd);
    if (rename(tmp, path)) slot_fail("cannot rename", path);
}

static void history(const char *event, const char *extra) {
    char p[PATH_MAX]; path_in(p, "history.log");
    int fd = open(p, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return;   /* the log is a convenience; the lock is what matters */
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    char line[1024];
    int n = snprintf(line, sizeof line, "%s %s owner=%s pid=%d mode=%s purpose=%s%s%s\n", ts, event, owner, getpid(),
                     mode_name, purpose, extra ? " " : "", extra ? extra : "");
    if (n > 0) (void)!write(fd, line, (size_t)(n < (int)sizeof line ? n : (int)sizeof line - 1));
    close(fd);
}

/* Process start time (seconds) or 0 if the pid is gone or a zombie; with the pid it names one process exactly. */
static uint64_t pid_start(pid_t pid) {
    struct proc_bsdinfo bi;
    if (pid <= 0 || proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != (int)sizeof bi || bi.pbi_status == SZOMB) return 0;
    return bi.pbi_start_tvsec;
}
static int pid_same(pid_t pid, uint64_t start) { return start && pid_start(pid) == start; }
static int pid_is_game(pid_t pid) {
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, path, sizeof path) <= 0) return 0;
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    return !strncmp(b, "totk-native", 11);   /* also renamed copies such as totk-native.base */
}

/* Live waiters in the queue (dead ones are removed). Returns how many; *head gets the oldest one's file name. */
static int queue_scan(char *head, size_t head_n, pid_t *pids, int max_pids) {
    char qdir[PATH_MAX]; path_in(qdir, "queue");
    DIR *d = opendir(qdir);
    if (!d) slot_fail("cannot open", qdir);
    int count = 0; if (head) head[0] = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strstr(e->d_name, ".tmp")) continue;
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s/%s", qdir, e->d_name) >= (int)sizeof p) continue;
        pid_t pid = (pid_t)kv_num(p, "pid");
        if (!pid_same(pid, (uint64_t)kv_num(p, "start"))) { unlink(p); continue; }
        if (pids && count < max_pids) pids[count] = pid;
        count++;
        if (head && (!head[0] || strcmp(e->d_name, head) < 0)) snprintf(head, head_n, "%s", e->d_name);
    }
    closedir(d);
    return count;
}

static pid_t user_request_pid(void) {   /* a live user game waiting for the slot, or 0 */
    char p[PATH_MAX]; path_in(p, "user-request");
    pid_t pid = (pid_t)kv_num(p, "pid");
    return pid_same(pid, (uint64_t)kv_num(p, "start")) ? pid : 0;
}

/* A totk-native process that is neither queued nor this one: a game from before the slot existed (or one using another
   slot directory). Only called while holding the lock, so no game that follows the protocol can be running. */
static pid_t legacy_game(void) {
    pid_t queued[64]; int nq = queue_scan(NULL, 0, queued, 64);
    pid_t user = user_request_pid();
    int n = proc_listallpids(NULL, 0);
    if (n <= 0) return 0;
    int cap = n + 64;
    pid_t *pids = calloc((size_t)cap, sizeof *pids);
    if (!pids) runtime_fail("game slot: out of memory");
    n = proc_listallpids(pids, cap * (int)sizeof *pids);
    pid_t found = 0;
    for (int i = 0; i < n && i < cap && !found; i++) {
        pid_t p = pids[i];
        if (p <= 0 || p == getpid() || p == user || !pid_is_game(p) || !pid_start(p)) continue;
        int q = 0;
        for (int k = 0; k < nq && k < 64; k++) q |= queued[k] == p;
        if (!q) found = p;
    }
    free(pids);
    return found;
}

static void describe_holder(char *out, size_t n) {
    char p[PATH_MAX]; path_in(p, "holder");
    char who[40] = "?", what[200] = "?";
    kv_get(p, "owner", who, sizeof who); kv_get(p, "purpose", what, sizeof what);
    long long since = kv_num(p, "since"), expect = kv_num(p, "expect_s"), gp = kv_num(p, "game_pid");
    long long held = since ? (long long)time(NULL) - since : 0;
    char left[64] = "";
    if (expect > held) snprintf(left, sizeof left, "，预计还要约 %lld 分钟", (expect - held + 59) / 60);
    snprintf(out, n, "%s（%s）占用中，已 %lld 分 %lld 秒%s，游戏 pid %lld", who, what, held / 60, held % 60, left, gp);
}

/* The user's game: stop an agent's game that holds the slot. The marker comes first, so the agent's script can tell a
   preemption from a crash; SIGKILL follows if the game has not gone 15 s after SIGTERM. */
static void preempt_holder(void) {
    static char done_token[48]; static time_t sent_at; static int killed;
    char p[PATH_MAX]; path_in(p, "holder");
    char who[40] = "", tok[48] = "", what[200] = "";
    kv_get(p, "owner", who, sizeof who); kv_get(p, "token", tok, sizeof tok); kv_get(p, "purpose", what, sizeof what);
    if (!tok[0] || !strcmp(who, "user")) return;   /* the user's own other game: wait for it */
    pid_t gp = (pid_t)kv_num(p, "game_pid");
    int live = pid_same(gp, (uint64_t)kv_num(p, "game_start")) && pid_is_game(gp);
    if (strcmp(done_token, tok)) {
        char m[PATH_MAX], name[80], text[200];
        snprintf(name, sizeof name, "preempted/%s", tok); path_in(m, name);
        snprintf(text, sizeof text, "by_pid=%d\nat=%lld\n", getpid(), (long long)time(NULL));
        write_atomic(m, text);
        char extra[300]; snprintf(extra, sizeof extra, "target_owner=%s target_pid=%d target_purpose=%s", who, gp, what);
        history("preempt", extra);
        if (live) kill(gp, SIGTERM);
        fprintf(stderr, "[game-slot] 用户要玩：停止 %s 的游戏（%s，pid %d），它的测试稍后会自动重跑\n", who, what, gp);
        snprintf(done_token, sizeof done_token, "%s", tok); sent_at = time(NULL); killed = 0;
    } else if (live && !killed && time(NULL) - sent_at > 15) {
        kill(gp, SIGKILL); killed = 1;
        fprintf(stderr, "[game-slot] pid %d 15 秒内没有退出，强制结束\n", gp);
    }
}

static void slot_dir_init(void) {
    char def[PATH_MAX] = "", exe[PROC_PIDPATHINFO_MAXSIZE];
    const char *env = getenv("TOTK_GAME_SLOT_DIR");
    /* Packaged launchers always supply an external slot. Do not create even
       an empty analysis directory inside a sealed or read-only app bundle. */
    if ((!env || !*env) && proc_pidpath(getpid(), exe, sizeof exe) > 0) {   /* <native-port>/build/totk-native -> <native-port>/analysis */
        char *s = strrchr(exe, '/'); if (s) *s = 0;
        s = strrchr(exe, '/'); if (s) *s = 0;
        char analysis[PATH_MAX], real[PATH_MAX];
        snprintf(analysis, sizeof analysis, "%s/analysis", exe);
        if (mkdir(analysis, 0755) && errno != EEXIST) slot_fail("cannot create", analysis);
        if (!realpath(analysis, real)) slot_fail("cannot resolve", analysis);
        snprintf(def, sizeof def, "%s/game-slot", real);
    }
    const char *want = env && *env ? env : def;
    if (!*want) runtime_fail("game slot: no slot directory (set TOTK_GAME_SLOT_DIR)");
    if (mkdir(want, 0755) && errno != EEXIST) slot_fail("cannot create", want);
    if (!realpath(want, slot_dir)) slot_fail("cannot resolve", want);
    char sub[PATH_MAX];
    path_in(sub, "queue"); if (mkdir(sub, 0755) && errno != EEXIST) slot_fail("cannot create", sub);
    path_in(sub, "preempted"); if (mkdir(sub, 0755) && errno != EEXIST) slot_fail("cannot create", sub);
    path_in(lock_path, "lock");
}

static int test_ignores_legacy(void) {   /* tests use their own slot directory; never the shared one */
    const char *e = getenv("TOTK_GAME_SLOT_TEST_IGNORE_LEGACY"), *d = getenv("TOTK_GAME_SLOT_DIR");
    if (!e || strcmp(e, "1")) return 0;
    if (!d || !*d || strstr(slot_dir, "/analysis/game-slot")) runtime_fail("TOTK_GAME_SLOT_TEST_IGNORE_LEGACY needs a test TOTK_GAME_SLOT_DIR");
    return 1;
}

static void holder_write(void) {
    char exe[PROC_PIDPATHINFO_MAXSIZE] = "", cwd[PATH_MAX] = "", log[PATH_MAX] = "", text[4096];
    proc_pidpath(getpid(), exe, sizeof exe);
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    if (fcntl(2, F_GETPATH, log) == -1) log[0] = 0;
    const char *w = getenv("TOTK_SLOT_WRAPPER_PID"), *ex = getenv("TOTK_SLOT_EXPECT_S"), *si = getenv("TOTK_SLOT_SINCE");
    snprintf(text, sizeof text,
             "owner=%s\npurpose=%s\ntoken=%s\ngame_pid=%d\ngame_start=%llu\nwrapper_pid=%s\nsince=%lld\nexpect_s=%s\nmode=%s\nexe=%s\ncwd=%s\nlog=%s\n",
             owner, purpose, token, getpid(), (unsigned long long)pid_start(getpid()), w ? w : "", si ? atoll(si) : (long long)acquired_at,
             ex ? ex : "0", mode_name, exe, cwd, log);
    char p[PATH_MAX]; path_in(p, "holder");
    write_atomic(p, text);
}

static void at_exit(void) {
    if (my_queue[0]) unlink(my_queue);
    if (!strcmp(owner, "user") && user_request_pid() == getpid()) { char p[PATH_MAX]; path_in(p, "user-request"); unlink(p); }
    if (acquired) { char extra[64]; snprintf(extra, sizeof extra, "held_s=%lld", (long long)(time(NULL) - acquired_at)); history("exit", extra); }
}

static void dry_run(void) {   /* tests: hold the slot like a game would, without booting */
    const char *e = getenv("TOTK_GAME_SLOT_DRYRUN");
    if (!e) return;
    double s = atof(e);
    fprintf(stderr, "[game-slot] dry run: holding the slot %.1f s\n", s);
    for (double t = 0; t < s; t += 0.05) usleep(50000);
    exit(0);
}

void runtime_game_slot_acquire(const char *mode) {
    clean_copy(mode_name, sizeof mode_name, mode);
    const char *o = getenv("TOTK_SLOT_OWNER"), *pu = getenv("TOTK_SLOT_PURPOSE");
    clean_copy(owner, sizeof owner, o && *o ? o : "unknown");
    for (const char *c = owner; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_'))
            runtime_fail("TOTK_SLOT_OWNER: use lower-case letters, digits, - or _ (user, claude, codex ...)");
    clean_copy(purpose, sizeof purpose, pu && *pu ? pu : mode);
    slot_dir_init();
    atexit(at_exit);

    const char *fd_env = getenv("TOTK_GAME_SLOT_FD");
    if (fd_env) {   /* a wrapper took the slot for us: check that this descriptor really holds the lock */
        int fd = atoi(fd_env); struct stat a, b;
        if (fd < 3 || fstat(fd, &a) || stat(lock_path, &b) || a.st_dev != b.st_dev || a.st_ino != b.st_ino)
            runtime_fail("TOTK_GAME_SLOT_FD is not the game slot lock of this slot directory");
        if (flock(fd, LOCK_EX | LOCK_NB)) runtime_fail("TOTK_GAME_SLOT_FD does not hold the game slot (another game has it)");
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        const char *t = getenv("TOTK_SLOT_TOKEN");
        if (!t || !*t) runtime_fail("TOTK_GAME_SLOT_FD without TOTK_SLOT_TOKEN");
        clean_copy(token, sizeof token, t);
        lock_fd = fd; acquired = 1; acquired_at = time(NULL);
        unsetenv("TOTK_GAME_SLOT_FD");
        holder_write();
        history("start", "slot=inherited");
        fprintf(stderr, "[game-slot] 游戏位置由启动脚本持有（%s，%s）\n", owner, purpose);
        dry_run();
        return;
    }

    lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock_fd < 0) slot_fail("cannot open", lock_path);
    int user = !strcmp(owner, "user"), ignore_legacy = test_ignores_legacy();
    const char *mw = getenv("TOTK_GAME_SLOT_WAIT");
    double max_wait = mw ? atof(mw) : -1;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    char name[80], text[600];
    snprintf(name, sizeof name, "queue/%019llu-%d", (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec, getpid());
    path_in(my_queue, name);
    snprintf(text, sizeof text, "pid=%d\nstart=%llu\nowner=%s\npurpose=%s\nsince=%lld\n", getpid(),
             (unsigned long long)pid_start(getpid()), owner, purpose, (long long)ts.tv_sec);
    write_atomic(my_queue, text);
    if (user) { char p[PATH_MAX]; path_in(p, "user-request"); write_atomic(p, text); }

    time_t start = time(NULL), last_log = 0;
    char reason[600] = "", key[128] = "", last_key[128] = "";   /* key: who blocks us, without the running time */
    for (;;) {
        char head[80]; queue_scan(head, sizeof head, NULL, 0);
        pid_t upid = user ? 0 : user_request_pid();
        const char *mine = strrchr(my_queue, '/') + 1;
        if (upid) { snprintf(reason, sizeof reason, "用户的游戏（pid %d）正在等，让它先", upid); snprintf(key, sizeof key, "user %d", upid); }
        else if (!user && strcmp(head, mine)) {
            snprintf(reason, sizeof reason, "排队中，前面还有别的测试（queue/%s）", head); snprintf(key, sizeof key, "queue %s", head);
        }
        else if (!flock(lock_fd, LOCK_EX | LOCK_NB)) {
            pid_t old = ignore_legacy ? 0 : legacy_game();
            if (!old) break;
            flock(lock_fd, LOCK_UN);
            snprintf(reason, sizeof reason, "pid %d 的游戏没有接入游戏锁（旧版程序？），等它退出", old);
            snprintf(key, sizeof key, "legacy %d", old);
        } else {
            if (errno != EWOULDBLOCK) slot_fail("flock", lock_path);
            describe_holder(reason, sizeof reason);
            char hp[PATH_MAX], tok[48] = ""; path_in(hp, "holder"); kv_get(hp, "token", tok, sizeof tok);
            snprintf(key, sizeof key, "holder %s", tok);
            if (user) preempt_holder();
        }
        time_t now = time(NULL);
        if (max_wait >= 0 && now - start >= max_wait) {
            fprintf(stderr, "[game-slot] 等了 %lld 秒还没轮到（%s）；TOTK_GAME_SLOT_WAIT=%s，退出 75\n", (long long)(now - start), reason, mw);
            exit(75);
        }
        if (strcmp(key, last_key) || now - last_log >= 30) {
            fprintf(stderr, "[game-slot] 等待游戏位置：%s\n", reason);
            snprintf(last_key, sizeof last_key, "%s", key); last_log = now;
        }
        usleep(user ? 200000 : 1000000);
    }
    unlink(my_queue); my_queue[0] = 0;
    if (user) { char p[PATH_MAX]; path_in(p, "user-request"); if (kv_num(p, "pid") == getpid()) unlink(p); }
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(token, sizeof token, "%llx-%d", (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec, getpid());
    acquired = 1; acquired_at = time(NULL);
    holder_write();
    char extra[64]; snprintf(extra, sizeof extra, "waited_s=%lld", (long long)(acquired_at - start));
    history("acquire", extra);
    fprintf(stderr, "[game-slot] 取得游戏位置（%s，%s）%s\n", owner, purpose, acquired_at > start ? "，等待结束" : "");
    dry_run();
}
#endif   /* !TARGET_OS_IPHONE */

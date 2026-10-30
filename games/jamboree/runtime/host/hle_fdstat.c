// Diagnostic DM_FD_LOG=<seconds>: every this many seconds log one line with "the number and breakdown of file descriptors the process has in use" (whether long play sessions will hit the open file limit: the iOS soft limit defaults to 256, raised to 4096 at startup in host_preload, main.c).
// fstat each descriptor to determine its type; for regular files / directories get the path with fcntl(F_GETPATH) and classify by where it points. Read-only, touches no descriptor; no thread is started when the variable is unset.
#include "hle.h"
#include "dm.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

static void *fdstat_main(void *arg) {
    int every = (int)(intptr_t)arg; char prev[1024] = "";
    for (unsigned round = 1;; round++) {
        sleep((unsigned)every);
        struct rlimit rl = {0}; getrlimit(RLIMIT_NOFILE, &rl); int max = getdtablesize(); if (max > 8192) max = 8192;
        int total = 0, romfs = 0, save = 0, logf = 0, cache = 0, dev = 0, sock = 0, pipe_ = 0, kq = 0, dir = 0, other = 0, hi = -1; char sample[400] = ""; int sk = 0;
        for (int fd = 0; fd < max; fd++) { struct stat st; if (fstat(fd, &st)) continue; total++; hi = fd;
            if (S_ISSOCK(st.st_mode)) { sock++; continue; } if (S_ISFIFO(st.st_mode)) { pipe_++; continue; } if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) { dev++; continue; }
            char p[MAXPATHLEN] = ""; if (fcntl(fd, F_GETPATH, p) || !p[0]) { if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) other++; else kq++; continue; }
            if (S_ISDIR(st.st_mode)) dir++;
            if (strstr(p, "romfs") || strstr(p, "RomFS")) romfs++;
            else if (strstr(p, "/save")) save++;
            else if (strstr(p, ".log") || strstr(p, "boot.txt") || strstr(p, ".jsonl") || strstr(p, ".tsv")) logf++;
            else if (strstr(p, "/Caches/") || strstr(p, "/cache/") || strstr(p, ".metallib") || strstr(p, "/tmp/") || strstr(p, "/var/folders/")) cache++;
            else { other++; if (sk < 300) { const char *b = strrchr(p, '/'); sk += snprintf(sample + sk, sizeof sample - sk, " %s", b ? b + 1 : p); } } }
        char line[1024]; snprintf(line, sizeof line, "in use %d (highest %d, soft limit %llu): resource archives %d, save data %d, logs %d, cache / temp %d, devices %d, sockets %d, pipes %d, other kernel objects %d, other files %d%s%s; of these directories %d",
                                   total, hi, (unsigned long long)rl.rlim_cur, romfs, save, logf, cache, dev, sock, pipe_, kq, other, other ? ":" : "", other ? sample : "", dir);
        LOG("descriptors round %u (every %d s) %s%s", round, every, line, strcmp(line, prev) ? "" : " (same as last time)"); strlcpy(prev, line, sizeof prev);
    }
    return NULL;
}
__attribute__((constructor)) static void fdstat_init(void) {
    const char *e = getenv("DM_FD_LOG"); int every = e ? atoi(e) : 0; if (every <= 0) return;
    pthread_t t; if (!pthread_create(&t, NULL, fdstat_main, (void *)(intptr_t)every)) pthread_detach(t);
}

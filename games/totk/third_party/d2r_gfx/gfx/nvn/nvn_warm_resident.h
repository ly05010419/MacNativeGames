// Which pipelines stay in memory after the warm-up (2026-11-16). Foundation only: nvn_mtl.m's warm_shared() and
// nvn_warm_policy.h include it; tests/warm_policy_probe.m runs it.
#pragma once
#import <Foundation/Foundation.h>
#include <stdlib.h>
#include <string.h>
#include <TargetConditionals.h>

/* When a pipeline archive holds the rest (warm_start), what is made at the warm-up and kept:
   ALL          every list (the Mac's default before 2026-11-16; TOTK_WARM_SHARED=1),
   TITLE_LOCAL  the title list and this device's list (iOS, b8cec411; TOTK_WARM_SHARED=0),
   TITLE_KINDS  the title list and every list's compute (C), mesh (M) and tile-read (T) rows, as a quick App warms them
                (games/oot/build.py title_shader_file); this device's list only fills the archive. User 2026-11-16 for
                Ocarina of Time's normal start ("普通启动 用 加上所有计算、网格、贴图读取管线 和 标题清单常驻", "不留 本机清单"):
                same save and route on an M4, peak footprint ALL 12.06 GiB, TITLE_LOCAL 9.38, the title list alone 8.90,
                TITLE_KINDS 9.06. Then the player's own run: the rest came from the archive on the submit thread in
                play (12 pipelines + 6 libraries in one 386 ms frame) and its footprint still reached 11.2-11.9 GiB, so
                ALL became Ocarina of Time's Mac default (user 2026-11-16: "改成 默认 常驻") and TITLE_KINDS its memory
                saver mode, MNG Launcher's "开启省内存模式". The same evening the memory saver became the default (user:
                "默认 打开省内存模式"): one autoload route on this 16 GB M4, 90 s in the world each, ALL footprint
                12.1-13.5 GB with 240,924 pages swapped out and 276 long frames (6.4 %, p99 59.2 ms), TITLE_KINDS
                9.7-9.9 GB, 11,384 pages and 35 long frames (0.7 %, p99 20.9 ms); its world entry took one 4.5 s frame
                (the libraries from the archive), ALL's 1.1 s. memory-saver.txt next to controls.txt says "off" to keep
                every list.
   TOTK_WARM_RESIDENT=all|title-local|title-kinds chooses; TOTK_WARM_SHARED=1/0 still means all / title-local. */
enum { WARM_RESIDENT_ALL, WARM_RESIDENT_TITLE_LOCAL, WARM_RESIDENT_TITLE_KINDS };
static const char *const WARM_RESIDENT_NAME[] = {"all", "title-local", "title-kinds"};

/* The mode the environment asks for, `fallback` when it asks for none, -1 for a value it does not know. */
__attribute__((unused)) static int warm_resident_parse(const char *resident, const char *shared, int fallback) {
    if (resident && *resident) {
        for (int m = 0; m < 3; m++) if (!strcmp(resident, WARM_RESIDENT_NAME[m])) return m;
        return -1;
    }
    if (shared && *shared) return atoi(shared) ? WARM_RESIDENT_ALL : WARM_RESIDENT_TITLE_LOCAL;
    return fallback;
}

/* memory-saver.txt's word (MNG Launcher writes "off" to keep every list, "on", or removes the file for the default): 1 on,
   0 off, WARM_SAVER_DEFAULT (on, user 2026-11-16 "默认 打开省内存模式") for no file or an empty one, -1 a word it does
   not know. Blanks and a newline around the word do not count. */
#define WARM_SAVER_FILE "memory-saver.txt"
#define WARM_SAVER_DEFAULT 1
__attribute__((unused)) static int warm_saver_word(const char *word) {
    if (!word) return WARM_SAVER_DEFAULT;
    while (*word == ' ' || *word == '\t') word++;
    size_t n = strlen(word);
    while (n && (word[n - 1] == '\n' || word[n - 1] == '\r' || word[n - 1] == ' ' || word[n - 1] == '\t')) n--;
    if (!n) return WARM_SAVER_DEFAULT;
    if (n == 3 && !strncmp(word, "off", 3)) return 0;
    if (n == 2 && !strncmp(word, "on", 2)) return 1;
    return -1;
}

/* The compute (C), mesh (M) and tile-read (T) rows of `lists`, each once, in list order (TITLE_KINDS keeps them). */
__attribute__((unused)) static NSArray<NSString *> *warm_kind_rows(NSArray<NSString *> *lists) {
    NSMutableArray<NSString *> *rows = [NSMutableArray array];
    NSMutableSet<NSString *> *seen = [NSMutableSet set];
    for (NSString *path in lists) @autoreleasepool {
        NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
        for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
            if (!([line hasPrefix:@"C\t"] || [line hasPrefix:@"M\t"] || [line hasPrefix:@"T\t"]) || [seen containsObject:line]) continue;
            [seen addObject:line]; [rows addObject:line];
        }
    }
    return rows;
}

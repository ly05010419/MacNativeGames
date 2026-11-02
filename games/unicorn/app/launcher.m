#import <Cocoa/Cocoa.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../runtime/game_version.h"

static int failed(NSString *detail) {
    [NSApplication sharedApplication];
    NSAlert *alert = [NSAlert new];
    alert.messageText = @"圣兽之王启动失败";
    alert.informativeText = detail;
    [alert runModal];
    return 1;
}

int main(void) {
    @autoreleasepool {
        NSString *work = [NSBundle.mainBundle objectForInfoDictionaryKey:@"UnicornWorkFolder"];
        if (!work || chdir(work.fileSystemRepresentation)) return failed(@"找不到游戏工作目录，请保留 game 文件夹中的游戏构建数据。");
        FILE *log = fopen("boot.log", "w");
        if (!log) return failed(@"无法写入工作目录中的 boot.log。");
        if (dup2(fileno(log), STDOUT_FILENO) < 0 || dup2(fileno(log), STDERR_FILENO) < 0) return failed(@"无法打开启动日志。");
        fclose(log);
        /* 图形层默认只编码命令；实际游戏启动必须明确启用 GPU 提交。 */
        setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1);
        setenv("TOTK_SLOT_OWNER", "user", 1);
        NSString *language = NSLocale.preferredLanguages.firstObject;
        if (language) setenv("TOTK_LOCALE", language.UTF8String, 1);
        unsetenv("TOTK_HEADLESS");
        NSString *binary = [work stringByAppendingPathComponent:@"build/unicorn-native"];
        char *const arguments[] = {(char *)binary.fileSystemRepresentation, "rewritten/" TOTK_GAME_DIR, "boot", NULL};
        execv(arguments[0], arguments);
        return failed([NSString stringWithFormat:@"%@：%s", binary, strerror(errno)]);
    }
}

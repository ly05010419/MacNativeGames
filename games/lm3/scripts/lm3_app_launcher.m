/* Self-contained LM3 launcher. Game resources are immutable; saves, imported
   shaders, verification receipts and locks belong to the current Mac user. */
#import <Foundation/Foundation.h>
#include "game_version_config.h"
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail(NSString *message) {
    fprintf(stderr, "[lm3-app] %s\n", message.UTF8String);
    return 1;
}
static void setting(const char *key, NSString *value) {
    if (setenv(key, value.fileSystemRepresentation, 1)) {
        perror(key); exit(1);
    }
}
static NSString *canonical_path(NSString *path) {
    NSMutableArray<NSString *> *missing = [NSMutableArray array];
    NSString *parent = path.stringByStandardizingPath;
    char resolved[4096];
    while (!realpath(parent.fileSystemRepresentation, resolved)) {
        if (errno != ENOENT || [parent isEqualToString:@"/"]) return nil;
        [missing addObject:parent.lastPathComponent];
        parent = parent.stringByDeletingLastPathComponent;
    }
    NSString *result = @(resolved);
    for (NSString *component in missing.reverseObjectEnumerator)
        result = [result stringByAppendingPathComponent:component];
    return result;
}
static NSString *external_directory(NSString *value, NSString *fallback, NSString *bundle) {
    NSString *path = value.length ? value : fallback;
    if (!path.isAbsolutePath) return nil;
    path = canonical_path(path);
    if (!path) return nil;
    if ([path isEqualToString:bundle] || [path hasPrefix:[bundle stringByAppendingString:@"/"]]) return nil;
    return path;
}

int main(int argc, const char **argv) { @autoreleasepool {
    NSDictionary<NSString *, NSString *> *inherited = NSProcessInfo.processInfo.environment;
    NSString *owner = [inherited[@"LM3_APP_OWNER"] isEqualToString:@"codex"] ? @"codex" : @"user";
    NSString *mode = @"boot";
    if (argc == 3 && !strcmp(argv[1], "--mode")) mode = @(argv[2]);
    else if (argc != 1 && !(argc == 2 && !strncmp(argv[1], "-psn_", 5)))
        return fail(@"usage: LM3 [--mode boot|probe|probe-gfx]");
    if (![@[@"boot", @"probe", @"probe-gfx"] containsObject:mode])
        return fail(@"unsupported validation mode");

    char executable[4096]; uint32_t length = sizeof executable;
    if (_NSGetExecutablePath(executable, &length)) return fail(@"launcher path is too long");
    NSString *self = canonical_path(@(executable));
    if (!self) return fail(@"cannot resolve the launcher path");
    NSString *contents = self.stringByDeletingLastPathComponent.stringByDeletingLastPathComponent;
    NSString *bundle = contents.stringByDeletingLastPathComponent;
    NSString *game = [contents stringByAppendingPathComponent:@"Resources/game"];
    NSString *native = [game stringByAppendingPathComponent:@"build/lm3-native"];
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *relative in @[@"build/lm3-native", @"build/shader-service/totk_xlate",
                                @"build/spirv-cross/spirv-cross"]) {
        if (![fm isExecutableFileAtPath:[game stringByAppendingPathComponent:relative]])
            return fail([@"missing executable in app: " stringByAppendingString:relative]);
    }
    NSString *base = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/MacNativeGames"];
    NSString *data = external_directory(inherited[@"LM3_APP_DATA_DIR"], [base stringByAppendingPathComponent:@"LM3"], bundle);
    NSString *logs = external_directory(inherited[@"LM3_APP_LOG_DIR"],
        [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/MacNativeGames"], bundle);
    if (!data || !logs) return fail(@"user data and logs require absolute paths outside the app");
    NSString *profile = [data stringByAppendingPathComponent:@"profile"];
    NSString *cache = [data stringByAppendingPathComponent:@"shader-cache"];
    NSString *proof = [data stringByAppendingPathComponent:@"romfs-proof/" TOTK_GAME_DIR];
    NSString *slot = inherited[@"LM3_APP_DATA_DIR"].length ? [data stringByAppendingPathComponent:@"game-slot"]
        : [base stringByAppendingPathComponent:@"game-slot"];
    NSError *error = nil;
    for (NSString *dir in @[data, logs, profile, cache, proof, slot]) {
        if (![fm createDirectoryAtPath:dir withIntermediateDirectories:YES
                            attributes:@{NSFilePosixPermissions:@0700} error:&error])
            return fail([NSString stringWithFormat:@"cannot create user storage: %@", error.localizedDescription]);
    }
    /* Discard development overrides, inherited locks and GPU experiments.
       Only these explicit launcher inputs are used; the runtime starts with
       the same defaults on a new user's Mac as from Finder on this Mac. */
    for (NSString *key in inherited) {
        if ([key hasPrefix:@"TOTK_"] || [key hasPrefix:@"DM_"] || [key hasPrefix:@"LM3_"] ||
            [key hasPrefix:@"DYLD_"] || [key hasPrefix:@"MTL_"])
            unsetenv(key.UTF8String);
    }
    setting("TOTK_SLOT_OWNER", owner);
    setting("TOTK_SLOT_PURPOSE", [owner isEqualToString:@"codex"] ? @"portable-validation" : @"standalone-lm3");
    setting("TOTK_PROFILE_DIR", profile); setting("TOTK_SHADER_CACHE", cache);
    setting("TOTK_ROMFS", [game stringByAppendingPathComponent:@"extracted/" TOTK_GAME_DIR "/romfs"]);
    setting("TOTK_SHARED_FONTS", [game stringByAppendingPathComponent:@"extracted/firmware/shared-fonts"]);
    setting("TOTK_SHADER_SEED_DIR", [game stringByAppendingPathComponent:@"build/shader-seed"]);
    setting("TOTK_ROMFS_PROOF_DIR", proof); setting("TOTK_GAME_SLOT_DIR", slot);
    setting("TOTK_ROMFS_PATCHES", @"0"); setting("TOTK_ROMFS_MODS", @"0");
    setting("TOTK_SHADER_SERVICE", @"native");
    setting("DM_NVN_SHMAP", [game stringByAppendingPathComponent:@"build/shader-map.tsv"]);
    setting("DM_MSL_DIR", [data stringByAppendingPathComponent:@"metal-libraries"]);
    for (NSString *key in @[@"TOTK_GPU_ALLOW_SUBMIT", @"TOTK_GPU_WIDEN", @"TOTK_GPU_BLOCKRT", @"TOTK_GPU_MIDCLEAR"])
        setting(key.UTF8String, @"1");
    setting("TOTK_GPU_BUDGET_MB", @"8000");
    if ([owner isEqualToString:@"codex"]) {
        if ([inherited[@"LM3_APP_SHADER_SEED"] isEqualToString:@"off"]) setting("TOTK_SHADER_SEED", @"off");
        setting("LM3_QUALITY_DIAG", @"1");
    }
    NSString *log = [logs stringByAppendingPathComponent:@"lm3.log"];
    NSString *previous = [logs stringByAppendingPathComponent:@"lm3.previous.log"];
    if ([fm fileExistsAtPath:log] && rename(log.fileSystemRepresentation, previous.fileSystemRepresentation))
        return fail(@"cannot rotate the previous game log");
    int fd = open(log.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return fail(@"cannot open the game log");
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) { close(fd); return fail(@"cannot redirect the game log"); }
    if (fd > STDERR_FILENO) close(fd);
    if (chdir(game.fileSystemRepresentation)) return fail(@"cannot enter the app's resource directory");
    fprintf(stderr, "[lm3-app] standalone %s; resources %s; profile %s; shader seed %s\n", TOTK_GAME_VERSION,
            game.fileSystemRepresentation, profile.fileSystemRepresentation,
            [game stringByAppendingPathComponent:@"build/shader-seed"].fileSystemRepresentation);
    NSString *rewritten = @"rewritten/" TOTK_GAME_DIR;
    const char *arguments[] = {native.fileSystemRepresentation, rewritten.UTF8String, mode.UTF8String, NULL};
    execv(arguments[0], (char *const *)arguments);
    return fail([NSString stringWithFormat:@"cannot start game: %s", strerror(errno)]);
} }

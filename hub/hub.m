// The hub app (build_app.py --hub compiles it as Contents/MacOS/MacNativeGames): one app with every game built into it.
// Each game is Contents/Resources/games/<id>/ (build_app.py step_hub):
//   game.json   {"game", "title", "version", "launcher", "root", "icon", "dlc"}: what the hub shows and how it starts the game
//   launcher    the game's own launcher (games/<game>/app/launcher.m), the same program a single-game app starts with
//   <root>/     the game's files, as Contents/Resources/<root> of a single-game app
// The hub shows the games; picking one (click twice, Return, or A on a controller) replaces the hub with that game's
// launcher (MNG_GAME_ROOT and MNG_GAME_VERSION tell it where the game is), so the game is this app while it runs. A small
// watcher (this program with --after) waits for the game to end and opens the hub again, unless the game's "Quit
// MacNativeGames" left MNG_HUB_QUIT_FILE (runtime/frontend.m, which also offers "Back to Game Hub" when MNG_HUB_APP is set).
// Each tile shows how far the game's startup shaders are: compiled into the app at build time (game.json "shaders"), or,
// once the game has run, how far its last warm-up got (the file game.json "status" names, written by the game).
// Below the games, "Game files" names the player's games folder (library.json, shared with build_app.py; Change...
// picks another, Show in Finder opens it) and lists what is in it, with what the last build found (library-files.json,
// build_app.py scan_library; each file's "adds": the versions it is part of): in the hub, not added yet, or not used and
// why. A file not in the hub yet has an Add button (so has its version's "+" pill on the game's card): it runs
// build_app.py --only <file> in Terminal, which builds the versions that file is part of (how is in library.json
// "builder", left by the last hub build), and quits; the build opens the hub again.
// Nothing here opens the player's game files, keys or saves: it lists the folder, and each launcher reads its game.
//   --check          (build_app.py, after packaging) every game's launcher --check; exits non-zero if one fails
//   MNG_GAME=<id>    starts that game without showing the hub, and the hub does not come back after it
//                    (open MacNativeGames.app --env MNG_GAME=totk-v143, or botw-v190)
#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#include <errno.h>
#include <signal.h>
#include <sys/event.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static BOOL chinese(void) { return [NSLocale.preferredLanguages.firstObject hasPrefix:@"zh"]; }
#define UI(zh, en) (chinese() ? @zh : @en)

@interface HubGame : NSObject
@property NSString *ident, *key, *title, *version, *launcher, *root, *shaders;   // key: which game (game.json "game")
@property NSArray<NSString *> *dlc;   // the add-on content packs in it (game.json "dlc")
@property NSImage *icon;
@property double shaderFraction;   // -1: nothing known
@end
@implementation HubGame
@end

static NSString *const kLastPlayed = @"LastPlayed", *const kLastGame = @"LastGame";

// ---- the games folder (build_app.py SUPPORT, DEFAULT_LIBRARY) ----

static NSString *support_dir(void) {   // MNG_SUPPORT_DIR: another one, for tests
    const char *dir = getenv("MNG_SUPPORT_DIR");
    if (dir && *dir) return @(dir);
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/MacNativeGames"];
}
static NSDictionary *read_json(NSString *path) {
    NSData *data = [NSData dataWithContentsOfFile:path];
    id value = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    return [value isKindOfClass:NSDictionary.class] ? value : @{};
}
static NSDictionary *library_settings(void) { return read_json([support_dir() stringByAppendingPathComponent:@"library.json"]); }
static NSString *library_folder(void) {
    NSString *folder = library_settings()[@"folder"];
    return [folder isKindOfClass:NSString.class] && folder.length ? folder : [NSHomeDirectory() stringByAppendingPathComponent:@"Games/MacNativeGames"];
}
static void library_save_folder(NSString *folder) {
    NSMutableDictionary *settings = [library_settings() mutableCopy];
    settings[@"folder"] = folder;
    [NSFileManager.defaultManager createDirectoryAtPath:support_dir() withIntermediateDirectories:YES attributes:nil error:nil];
    [[NSJSONSerialization dataWithJSONObject:settings options:NSJSONWritingPrettyPrinted error:nil]
        writeToFile:[support_dir() stringByAppendingPathComponent:@"library.json"] atomically:YES];
}
static NSString *shell_quote(NSString *s) {
    return [NSString stringWithFormat:@"'%@'", [s stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"]];
}

static NSNumber *number(id v) { return [v isKindOfClass:NSNumber.class] ? v : nil; }

/* The tile's shader line. After a start of this build of the game: how far its warm-up got (it writes the status file
   at each percent and when done; a file older than the game's game.json is from an earlier build and not used).
   Before that: the share of startup shaders compiled into the app when it was built (the rest compile on the first
   start). */
static void hub_shader_state(HubGame *g, NSDictionary *info, NSString *folder) {
    g.shaderFraction = -1;
    NSFileManager *fm = NSFileManager.defaultManager;
    if ([info[@"status"] isKindOfClass:NSString.class]) {
        NSString *path = [support_dir()
                          stringByAppendingPathComponent:info[@"status"]];
        NSDate *built = [fm attributesOfItemAtPath:[folder stringByAppendingPathComponent:@"game.json"] error:nil].fileModificationDate;
        NSDate *written = [fm attributesOfItemAtPath:path error:nil].fileModificationDate;
        NSData *data = written && built && [written compare:built] != NSOrderedAscending ? [NSData dataWithContentsOfFile:path] : nil;
        NSDictionary *status = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        NSNumber *percent = [status isKindOfClass:NSDictionary.class] ? number(status[@"percent"]) : nil;
        if (percent) {
            int p = MAX(0, MIN(100, percent.intValue));
            g.shaderFraction = p / 100.0;
            g.shaders = [number(status[@"finished"]) boolValue] ? UI("着色器 100% 就绪", "Shaders 100% ready")
                      : [NSString stringWithFormat:UI("着色器 %d%% · 下次启动继续", "Shaders %d%% · continues next start"), p];
            return;
        }
    }
    NSDictionary *built = [info[@"shaders"] isKindOfClass:NSDictionary.class] ? info[@"shaders"] : nil;
    NSNumber *total = number(built[@"total"]), *compiled = number(built[@"compiled"]);
    if (total.doubleValue > 0 && compiled) {
        int p = (int)floor(MIN(1.0, compiled.doubleValue / total.doubleValue) * 100);
        g.shaderFraction = p / 100.0;
        g.shaders = p == 100 ? UI("着色器已全部编译", "Shaders 100% compiled")
                  : [NSString stringWithFormat:UI("着色器已编译 %d%% · 其余首次启动时编译", "Shaders %d%% compiled · rest at first start"), p];
    }
}

/* The games in this app, by title, newest version first. */
static NSArray<HubGame *> *hub_games(void) {
    NSString *dir = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"games"];
    NSMutableArray<HubGame *> *games = [NSMutableArray array];
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil]) {
        NSString *folder = [dir stringByAppendingPathComponent:name];
        NSData *json = [NSData dataWithContentsOfFile:[folder stringByAppendingPathComponent:@"game.json"]];
        NSDictionary *info = json ? [NSJSONSerialization JSONObjectWithData:json options:0 error:nil] : nil;
        if (![info isKindOfClass:NSDictionary.class] || ![info[@"title"] isKindOfClass:NSString.class] ||
            ![info[@"launcher"] isKindOfClass:NSString.class] || ![info[@"root"] isKindOfClass:NSString.class]) continue;
        HubGame *g = [HubGame new];
        g.ident = name; g.title = info[@"title"];
        g.key = [info[@"game"] isKindOfClass:NSString.class] ? info[@"game"] : g.title;
        g.version = [info[@"version"] isKindOfClass:NSString.class] ? info[@"version"] : @"";
        g.dlc = [info[@"dlc"] isKindOfClass:NSArray.class] ? info[@"dlc"] : @[];
        g.launcher = [folder stringByAppendingPathComponent:info[@"launcher"]];
        g.root = [folder stringByAppendingPathComponent:info[@"root"]];
        if ([info[@"icon"] isKindOfClass:NSString.class])
            g.icon = [[NSImage alloc] initWithContentsOfFile:[folder stringByAppendingPathComponent:info[@"icon"]]];
        if (!g.icon) g.icon = [NSImage imageNamed:NSImageNameApplicationIcon];
        hub_shader_state(g, info, folder);
        [games addObject:g];
    }
    [games sortUsingComparator:^NSComparisonResult(HubGame *a, HubGame *b) {
        NSComparisonResult t = [a.title localizedCaseInsensitiveCompare:b.title];
        return t != NSOrderedSame ? t : [b.version compare:a.version options:NSNumericSearch];
    }];
    return games;
}

static void hub_set_env(HubGame *g) {
    setenv("MNG_GAME_ROOT", g.root.fileSystemRepresentation, 1);
    setenv("MNG_GAME_VERSION", g.version.UTF8String, 1);
}

/* --check: run each game's launcher --check (it looks that its game is there and is the version it was built for). */
static int hub_check(void) {
    NSArray<HubGame *> *games = hub_games();
    int failed = !games.count;
    if (!games.count) printf("no games in this app\n");
    for (HubGame *g in games) {
        hub_set_env(g);
        char *const arguments[] = {(char *)g.launcher.fileSystemRepresentation, "--check", NULL};
        pid_t pid; int status = 1;
        int spawned = posix_spawn(&pid, arguments[0], NULL, NULL, arguments, environ) == 0;
        if (spawned) while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        int ok = spawned && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        printf("%s %s: %s\n", g.ident.UTF8String, g.version.UTF8String, ok ? "ok" : "failed");
        failed |= !ok;
    }
    fflush(stdout);
    return failed;
}

/* --after <pid> <quit file>: wait for the game (the hub's own process, which became the game) to end, then open the hub
   again, unless the game left the quit file. */
static int hub_after(pid_t pid, const char *quitFile) {
    int kq = kqueue();
    struct kevent ev;
    EV_SET(&ev, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    if (kq >= 0 && kevent(kq, &ev, 1, NULL, 0, NULL) == 0) {
        struct kevent out;
        while (kevent(kq, NULL, 0, &out, 1, NULL) < 0 && errno == EINTR) {}
    } else if (errno != ESRCH) return 1;   // ESRCH: already gone
    if (quitFile && access(quitFile, F_OK) == 0) { unlink(quitFile); return 0; }
    /* LaunchServices lets go of the app a moment after its process ends; until then open would only try to bring it
       forward. */
    for (int i = 0; i < 50 && [NSRunningApplication runningApplicationWithProcessIdentifier:pid]; i++) usleep(100000);
    NSString *app = NSBundle.mainBundle.bundlePath;
    char *const arguments[] = {"/usr/bin/open", (char *)app.fileSystemRepresentation, NULL};
    pid_t child; int status;
    if (posix_spawn(&child, arguments[0], NULL, NULL, arguments, environ) != 0) return 1;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return 0;
}

/* The watcher for hub_after, in its own session so it outlives the game and its signals. Returns its pid, or 0. */
static pid_t hub_watch(NSString *quitFile) {
    char self[32]; snprintf(self, sizeof self, "%d", getpid());
    char *const arguments[] = {(char *)NSBundle.mainBundle.executablePath.fileSystemRepresentation, "--after", self,
                               (char *)quitFile.fileSystemRepresentation, NULL};
    posix_spawnattr_t attr; posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    pid_t pid = 0;
    if (posix_spawn(&pid, arguments[0], NULL, &attr, arguments, environ) != 0) pid = 0;
    posix_spawnattr_destroy(&attr);
    return pid;
}

/* Replaces the hub with the game's launcher; returns only if that failed. comeBack: the hub opens again when the game
   ends. */
static void hub_launch(HubGame *g, BOOL comeBack) {
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    NSMutableDictionary *played = [[d dictionaryForKey:kLastPlayed] mutableCopy] ?: [NSMutableDictionary dictionary];
    played[g.ident] = NSDate.date;
    [d setObject:played forKey:kLastPlayed];
    [d setObject:g.ident forKey:kLastGame];
    NSMutableDictionary *chosen = [[d dictionaryForKey:@"ChosenVersion"] mutableCopy] ?: [NSMutableDictionary dictionary];
    chosen[g.key ?: g.title] = g.version;   // the card shows this version next time (HubGrid kChosen)
    [d setObject:chosen forKey:@"ChosenVersion"];
    [d synchronize];   // this process image ends at execv
    hub_set_env(g);
    pid_t watcher = 0;
    if (comeBack) {
        NSString *quitFile = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"mng-hub-%d.quit", getpid()]];
        unlink(quitFile.fileSystemRepresentation);
        setenv("MNG_HUB_APP", NSBundle.mainBundle.bundlePath.fileSystemRepresentation, 1);
        setenv("MNG_HUB_QUIT_FILE", quitFile.fileSystemRepresentation, 1);
        watcher = hub_watch(quitFile);
    }
    char *const arguments[] = {(char *)g.launcher.fileSystemRepresentation, NULL};
    execv(arguments[0], arguments);
    int error = errno;
    if (watcher) kill(watcher, SIGKILL);
    unsetenv("MNG_GAME_ROOT"); unsetenv("MNG_GAME_VERSION"); unsetenv("MNG_HUB_APP"); unsetenv("MNG_HUB_QUIT_FILE");
    NSAlert *alert = [NSAlert new];
    alert.messageText = [NSString stringWithFormat:UI("无法启动 %@", "%@ could not start"), g.title];
    alert.informativeText = [NSString stringWithFormat:@"%@: %s", g.launcher, strerror(error)];
    [alert runModal];
}

// ---- the window: one card per game, its versions as pills ----

enum { CARD_W = 300, CARD_H = 476, ICON = 220, GAP = 36, MARGIN = 40, PILL_H = 26, PLAY_H = 38 };
static NSString *const kChosen = @"ChosenVersion";   // game key -> the version last picked on its card

/* One game: its versions in the hub and, as options without a launcher, the versions found in the games folder that
   are not built in yet; newest first. */
@interface HubTitle : NSObject
@property NSString *key, *title;
@property NSImage *icon;
@property NSMutableArray<HubGame *> *options;
@property NSInteger chosen;
@end
@implementation HubTitle
@end

@interface HubGrid : NSView
@property (nonatomic) NSArray<HubGame *> *games;
@property NSArray<HubTitle *> *titles;
@property NSInteger selected;
@property (copy) void (^onAdd)(HubGame *);   // a version that is not in the hub yet: build it from its file
@property BOOL canAdd;
@end

@implementation HubGrid
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }

/* Groups the games by game; folder: game key -> versions found in the games folder (library-files.json). */
- (void)setGames:(NSArray<HubGame *> *)games folder:(NSDictionary<NSString *, NSSet<NSString *> *> *)folder {
    _games = games;
    NSString *keepKey = self.titles.count ? self.titles[self.selected].key : nil;
    NSMutableDictionary<NSString *, HubTitle *> *byKey = [NSMutableDictionary dictionary];
    NSMutableArray<HubTitle *> *titles = [NSMutableArray array];
    for (HubGame *g in games) {
        HubTitle *t = byKey[g.key];
        if (!t) { t = [HubTitle new]; t.key = g.key; t.title = g.title; t.icon = g.icon; t.options = [NSMutableArray array]; byKey[g.key] = t; [titles addObject:t]; }
        [t.options addObject:g];
    }
    [folder enumerateKeysAndObjectsUsingBlock:^(NSString *key, NSSet<NSString *> *versions, BOOL *stop) { (void)stop;
        HubTitle *t = byKey[key];
        if (!t) return;   // a game with no version in the hub yet has no title or icon to show
        for (NSString *v in versions) {
            if ([[t.options valueForKey:@"version"] containsObject:v]) continue;
            HubGame *pending = [HubGame new]; pending.key = key; pending.title = t.title; pending.version = v;
            [t.options addObject:pending];
        }
    }];
    NSDictionary *chosen = [NSUserDefaults.standardUserDefaults dictionaryForKey:kChosen];
    NSString *last = [NSUserDefaults.standardUserDefaults stringForKey:kLastGame];
    for (HubTitle *t in titles) {
        [t.options sortUsingComparator:^NSComparisonResult(HubGame *a, HubGame *b) { return [b.version compare:a.version options:NSNumericSearch]; }];
        t.chosen = 0;
        for (NSInteger i = 0; i < (NSInteger)t.options.count; i++)
            if ([t.options[i].version isEqual:chosen[t.key]] || [t.options[i].ident isEqual:last]) t.chosen = i;
    }
    [titles sortUsingComparator:^NSComparisonResult(HubTitle *a, HubTitle *b) { return [a.title localizedCaseInsensitiveCompare:b.title]; }];
    self.titles = titles;
    self.selected = 0;
    for (NSInteger i = 0; i < (NSInteger)titles.count; i++)
        if ([titles[i].key isEqual:keepKey] || (!keepKey && [[titles[i].options valueForKey:@"ident"] containsObject:last ?: @""])) self.selected = i;
    [self fit];
}

- (NSRect)cardRect:(NSInteger)i {
    NSInteger n = self.titles.count;
    CGFloat used = n * CARD_W + (n - 1) * GAP, left = MAX(MARGIN, (NSWidth(self.bounds) - used) / 2);
    return NSMakeRect(left + i * (CARD_W + GAP), MAX(8, (NSHeight(self.bounds) - CARD_H) / 2), CARD_W, CARD_H);
}
- (CGFloat)titleY:(NSRect)r { return NSMinY(r) + 24 + ICON + 14; }
- (NSArray<NSValue *> *)pillRects:(NSInteger)i {
    NSRect r = [self cardRect:i];
    HubTitle *t = self.titles[i];
    NSDictionary *style = @{NSFontAttributeName: [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold]};
    NSMutableArray<NSNumber *> *widths = [NSMutableArray array]; CGFloat total = 0;
    for (HubGame *g in t.options) {
        CGFloat w = ceil([[self pillText:g] sizeWithAttributes:style].width) + 24;
        [widths addObject:@(w)]; total += w;
    }
    total += 8 * (MAX(1, (NSInteger)t.options.count) - 1);
    CGFloat x = NSMidX(r) - total / 2, y = [self titleY:r] + 36;
    NSMutableArray<NSValue *> *rects = [NSMutableArray array];
    for (NSNumber *w in widths) { [rects addObject:[NSValue valueWithRect:NSMakeRect(x, y, w.doubleValue, PILL_H)]]; x += w.doubleValue + 8; }
    return rects;
}
- (NSRect)playRect:(NSInteger)i {
    NSRect r = [self cardRect:i];
    return NSMakeRect(NSMinX(r) + 24, NSMaxY(r) - 22 - PLAY_H, CARD_W - 48, PLAY_H);
}
- (NSString *)pillText:(HubGame *)g { return g.launcher ? g.version : [@"+ " stringByAppendingString:g.version]; }

/* Width fits every card in one row (scrolling sideways when they do not fit); height is the visible height. */
- (void)fit {
    NSSize visible = self.enclosingScrollView.contentSize;
    NSInteger n = self.titles.count;
    [self setFrameSize:NSMakeSize(MAX(visible.width, 2 * MARGIN + n * CARD_W + MAX(0, n - 1) * GAP), visible.height)];
    self.needsDisplay = YES;
}

- (NSString *)playedText:(HubGame *)g {
    NSDate *when = [NSUserDefaults.standardUserDefaults dictionaryForKey:kLastPlayed][g.ident];
    if (![when isKindOfClass:NSDate.class]) return UI("还没玩过", "Not played yet");
    NSDateFormatter *f = [NSDateFormatter new];
    f.dateStyle = NSDateFormatterMediumStyle; f.timeStyle = NSDateFormatterNoStyle; f.doesRelativeDateFormatting = YES;
    return [NSString stringWithFormat:UI("上次游玩：%@", "Last played %@"), [f stringFromDate:when]];
}

- (void)drawRect:(NSRect)dirty {
    NSMutableParagraphStyle *center = [NSMutableParagraphStyle new];
    center.alignment = NSTextAlignmentCenter; center.lineBreakMode = NSLineBreakByTruncatingTail;
    NSDictionary *titleStyle = @{NSFontAttributeName: [NSFont systemFontOfSize:18 weight:NSFontWeightSemibold],
                                 NSForegroundColorAttributeName: NSColor.labelColor, NSParagraphStyleAttributeName: center};
    NSDictionary *detailStyle = @{NSFontAttributeName: [NSFont systemFontOfSize:12],
                                  NSForegroundColorAttributeName: NSColor.secondaryLabelColor, NSParagraphStyleAttributeName: center};
    for (NSInteger i = 0; i < (NSInteger)self.titles.count; i++) {
        NSRect r = [self cardRect:i];
        if (!NSIntersectsRect(r, dirty)) continue;
        HubTitle *t = self.titles[i];
        HubGame *g = t.options[t.chosen];
        BOOL on = i == self.selected;
        NSBezierPath *card = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, 2, 2) xRadius:18 yRadius:18];
        [(on ? [NSColor.controlAccentColor colorWithAlphaComponent:0.16] : [NSColor.labelColor colorWithAlphaComponent:0.05]) setFill];
        [card fill];
        if (on) { [NSColor.controlAccentColor setStroke]; card.lineWidth = 3; [card stroke]; }

        NSRect icon = NSMakeRect(NSMidX(r) - ICON / 2.0, NSMinY(r) + 24, ICON, ICON);
        [NSGraphicsContext saveGraphicsState];
        [[NSBezierPath bezierPathWithRoundedRect:icon xRadius:30 yRadius:30] addClip];
        [t.icon drawInRect:icon fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
        [NSGraphicsContext restoreGraphicsState];
        CGFloat y = [self titleY:r];
        [t.title drawInRect:NSMakeRect(NSMinX(r) + 12, y, CARD_W - 24, 26) withAttributes:titleStyle];

        NSArray<NSValue *> *pills = [self pillRects:i];
        for (NSInteger k = 0; k < (NSInteger)pills.count; k++) {
            HubGame *option = t.options[k];
            NSRect p = pills[k].rectValue;
            BOOL picked = k == t.chosen, playable = option.launcher != nil;
            NSBezierPath *pill = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(p, 0.75, 0.75) xRadius:PILL_H / 2.0 yRadius:PILL_H / 2.0];
            NSColor *ink = playable ? NSColor.labelColor : NSColor.tertiaryLabelColor;
            if (picked && playable) { [NSColor.controlAccentColor setFill]; [pill fill]; ink = NSColor.whiteColor; }
            else {
                if (picked) { [[NSColor.labelColor colorWithAlphaComponent:0.10] setFill]; [pill fill]; ink = NSColor.labelColor; }
                if (!playable) { CGFloat dash[] = {4, 3}; [pill setLineDash:dash count:2 phase:0]; }
                [(playable ? [NSColor.labelColor colorWithAlphaComponent:0.35] : NSColor.tertiaryLabelColor) setStroke];
                pill.lineWidth = 1.5; [pill stroke];
            }
            NSDictionary *pillStyle = @{NSFontAttributeName: [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold],
                                        NSForegroundColorAttributeName: ink, NSParagraphStyleAttributeName: center};
            [[self pillText:option] drawInRect:NSMakeRect(NSMinX(p), NSMinY(p) + 4, NSWidth(p), PILL_H - 4) withAttributes:pillStyle];
        }

        CGFloat info = [self titleY:r] + 36 + PILL_H + 14;
        if (g.launcher) {
            [[self playedText:g] drawInRect:NSMakeRect(NSMinX(r) + 12, info, CARD_W - 24, 18) withAttributes:detailStyle];
            if (g.shaders) {
                [g.shaders drawInRect:NSMakeRect(NSMinX(r) + 8, info + 20, CARD_W - 16, 18) withAttributes:detailStyle];
                NSRect bar = NSMakeRect(NSMinX(r) + 40, info + 42, CARD_W - 80, 5);
                [[NSColor.labelColor colorWithAlphaComponent:0.12] setFill];
                [[NSBezierPath bezierPathWithRoundedRect:bar xRadius:2.5 yRadius:2.5] fill];
                bar.size.width *= MAX(0, g.shaderFraction);
                [(g.shaderFraction >= 1 ? NSColor.systemGreenColor : NSColor.controlAccentColor) setFill];
                [[NSBezierPath bezierPathWithRoundedRect:bar xRadius:2.5 yRadius:2.5] fill];
            }
        } else {
            [UI("在你的游戏文件夹里，还没加入大厅", "In your games folder, not in the hub yet")
                drawInRect:NSMakeRect(NSMinX(r) + 12, info, CARD_W - 24, 18) withAttributes:detailStyle];
        }

        NSRect play = [self playRect:i];
        BOOL can = g.launcher || self.canAdd;
        NSBezierPath *button = [NSBezierPath bezierPathWithRoundedRect:play xRadius:PLAY_H / 2.0 yRadius:PLAY_H / 2.0];
        [(on && can ? NSColor.controlAccentColor : [NSColor.labelColor colorWithAlphaComponent:can ? 0.10 : 0.05]) setFill];
        [button fill];
        NSString *label = g.launcher ? [NSString stringWithFormat:UI("▶  开始 %@", "▶  Play %@"), g.version]
                                     : [NSString stringWithFormat:UI("+  把 %@ 加入大厅", "+  Add %@ to the hub"), g.version];
        NSDictionary *playStyle = @{NSFontAttributeName: [NSFont systemFontOfSize:14 weight:NSFontWeightSemibold],
                                    NSForegroundColorAttributeName: on && can ? NSColor.whiteColor : can ? NSColor.labelColor : NSColor.tertiaryLabelColor,
                                    NSParagraphStyleAttributeName: center};
        [label drawInRect:NSMakeRect(NSMinX(play), NSMinY(play) + 10, NSWidth(play), PLAY_H - 10) withAttributes:playStyle];
    }
}

- (void)select:(NSInteger)i {
    if (!self.titles.count) return;
    i = MAX(0, MIN((NSInteger)self.titles.count - 1, i));
    self.selected = i;
    [self scrollRectToVisible:NSInsetRect([self cardRect:i], -GAP / 2, 0)];
    self.needsDisplay = YES;
}
- (void)choose:(NSInteger)k {
    if (!self.titles.count) return;
    HubTitle *t = self.titles[self.selected];
    t.chosen = MAX(0, MIN((NSInteger)t.options.count - 1, k));
    NSMutableDictionary *chosen = [[NSUserDefaults.standardUserDefaults dictionaryForKey:kChosen] mutableCopy] ?: [NSMutableDictionary dictionary];
    chosen[t.key] = t.options[t.chosen].version;
    [NSUserDefaults.standardUserDefaults setObject:chosen forKey:kChosen];
    self.needsDisplay = YES;
}
/* dx: another game; dy: another version of this one. */
- (void)move:(NSInteger)dx dy:(NSInteger)dy {
    if (dx) [self select:self.selected + dx];
    if (dy && self.titles.count) [self choose:self.titles[self.selected].chosen + dy];
}
- (void)play {
    if (!self.titles.count) return;
    HubTitle *t = self.titles[self.selected];
    HubGame *g = t.options[t.chosen];
    if (g.launcher) hub_launch(g, YES);
    else if (self.canAdd && self.onAdd) self.onAdd(g);
    else NSBeep();
}

- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    for (NSInteger i = 0; i < (NSInteger)self.titles.count; i++) {
        if (!NSPointInRect(p, [self cardRect:i])) continue;
        [self select:i];
        NSArray<NSValue *> *pills = [self pillRects:i];
        for (NSInteger k = 0; k < (NSInteger)pills.count; k++)
            if (NSPointInRect(p, NSInsetRect(pills[k].rectValue, -3, -4))) { [self choose:k]; return; }
        if (NSPointInRect(p, [self playRect:i]) || event.clickCount >= 2) [self play];
        return;
    }
}
- (void)keyDown:(NSEvent *)event {
    switch (event.keyCode) {
        case 123: [self move:-1 dy:0]; break;   // arrows: left / right another game, up / down another version
        case 124: [self move:1 dy:0]; break;
        case 125: [self move:0 dy:1]; break;
        case 126: [self move:0 dy:-1]; break;
        case 48: [self move:0 dy:(event.modifierFlags & NSEventModifierFlagShift) ? -1 : 1]; break;   // Tab
        case 36: case 76: case 49: [self play]; break;   // Return, Enter, Space
        default: [super keyDown:event];
    }
}
@end

// ---- controller: d-pad or left stick left / right for another game, up / down or L / R for another version (held:
// repeats), A (Switch layout by position: the right button) or the bottom button to play ----

@interface HubPad : NSObject
@property (weak) HubGrid *grid;
@property NSInteger heldX, heldY;
@property CFTimeInterval nextRepeat;
@property BOOL confirmDown, shoulderDown;
@end

@implementation HubPad
- (void)poll {
    if (!NSApp.isActive) return;
    NSInteger x = 0, y = 0, shoulder = 0; BOOL confirm = NO;
    for (GCController *c in GCController.controllers) {
        GCExtendedGamepad *pad = c.extendedGamepad;
        if (!pad) continue;
        float sx = pad.dpad.xAxis.value + pad.leftThumbstick.xAxis.value, sy = pad.dpad.yAxis.value + pad.leftThumbstick.yAxis.value;
        if (sx > 0.5f) x = 1; else if (sx < -0.5f) x = -1;
        if (sy > 0.5f) y = -1; else if (sy < -0.5f) y = 1;   // stick up = the version on the left (newer)
        if (pad.leftShoulder.isPressed) shoulder = -1; else if (pad.rightShoulder.isPressed) shoulder = 1;
        confirm |= pad.buttonA.isPressed || pad.buttonB.isPressed;
    }
    CFTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    if (x != self.heldX || y != self.heldY) {
        self.heldX = x; self.heldY = y;
        if (x || y) { [self.grid move:x dy:y]; self.nextRepeat = now + 0.4; }
    } else if ((x || y) && now >= self.nextRepeat) {
        [self.grid move:x dy:y]; self.nextRepeat = now + 0.13;
    }
    if (shoulder && !self.shoulderDown) [self.grid move:0 dy:shoulder];
    self.shoulderDown = shoulder != 0;
    if (confirm && !self.confirmDown) [self.grid play];   // on press, not while held from before
    self.confirmDown = confirm;
}
@end

@interface HubDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property NSWindow *window;
@property HubGrid *grid;
@property HubPad *pad;
@property NSTextField *folderLabel;
@property NSStackView *files;
@property NSMutableArray<NSString *> *addable;   // the Add buttons' files (button tag: index)
@property NSMutableDictionary<NSString *, NSString *> *pendingFiles;   // "<game key> <version>" -> its file, for the cards
@end

@interface HubFlipped : NSView   // the game file rows, from the top
@end
@implementation HubFlipped
- (BOOL)isFlipped { return YES; }
@end

@implementation HubDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }
- (void)applicationDidBecomeActive:(NSNotification *)note { [self refreshLibrary]; }   // files dropped in from Finder

/* The games folder's game files and keys, with what the last build found for each (library-files.json; a file whose
   size or date changed since counts as new). */
- (void)refreshLibrary {
    if (!self.files) return;
    NSString *folder = library_folder();
    NSFileManager *fm = NSFileManager.defaultManager;
    self.folderLabel.stringValue = [folder stringByAbbreviatingWithTildeInPath];
    NSDictionary *index = read_json([support_dir() stringByAppendingPathComponent:@"library-files.json"]);
    NSMutableDictionary<NSString *, NSDictionary *> *known = [NSMutableDictionary dictionary];
    if ([index[@"folder"] isKindOfClass:NSString.class] &&
        [[index[@"folder"] stringByStandardizingPath] isEqualToString:folder.stringByStandardizingPath] &&
        [index[@"files"] isKindOfClass:NSArray.class])
        for (NSDictionary *row in index[@"files"])
            if ([row isKindOfClass:NSDictionary.class] && [row[@"name"] isKindOfClass:NSString.class]) known[row[@"name"]] = row;
    NSMutableSet<NSString *> *inHub = [NSMutableSet set];   // "<game key> <version>", and "<game key> <version> dlc <pack>"
    for (HubGame *g in self.grid.games) {
        [inHub addObject:[NSString stringWithFormat:@"%@ %@", g.key, g.version]];
        for (id pack in g.dlc) [inHub addObject:[NSString stringWithFormat:@"%@ %@ dlc %@", g.key, g.version, pack]];
    }
    NSMutableDictionary<NSString *, NSMutableSet<NSString *> *> *found = [NSMutableDictionary dictionary];   // for the cards' pills

    NSDictionary *builder = [library_settings()[@"builder"] isKindOfClass:NSDictionary.class] ? library_settings()[@"builder"] : nil;
    BOOL canBuild = [builder[@"script"] isKindOfClass:NSString.class] && [fm isExecutableFileAtPath:builder[@"python"]] &&
                    [fm fileExistsAtPath:builder[@"script"]];
    BOOL keys = NO;
    for (NSString *name in [fm contentsOfDirectoryAtPath:folder error:nil])
        if ([name.pathExtension.lowercaseString isEqualToString:@"keys"]) keys = YES;
    NSString *cannot = !canBuild ? UI("找不到上次打包用的 build_app.py：在终端里运行 python3 build_app.py --hub",
                                      "The build_app.py of the last build is not found: run python3 build_app.py --hub in Terminal")
                     : !keys ? UI("先把 prod.keys 放进这个文件夹", "Put prod.keys in this folder first") : nil;
    for (NSView *v in self.files.arrangedSubviews.copy) [v removeFromSuperview];
    self.addable = [NSMutableArray array];
    self.pendingFiles = [NSMutableDictionary dictionary];
    NSFont *font = [NSFont systemFontOfSize:12];
    /* One line: mark, status, the file's name (shortened in the middle when long), and Add for a file to build. */
    void (^line)(NSString *, NSColor *, NSString *, NSString *, NSString *) = ^(NSString *mark, NSColor *color, NSString *status,
                                                                             NSString *name, NSString *add) {
        NSTextField *m = [self label:mark size:12 weight:NSFontWeightBold color:color];
        NSTextField *st = [self label:status size:12 weight:NSFontWeightRegular color:NSColor.labelColor];
        NSTextField *n = [self label:name ?: @"" size:12 weight:NSFontWeightRegular color:NSColor.secondaryLabelColor];
        n.font = font; n.lineBreakMode = NSLineBreakByTruncatingMiddle;
        [n setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
        [n setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
        NSMutableArray<NSView *> *views = [NSMutableArray arrayWithObjects:m, st, n, nil];
        if (add) {
            NSButton *b = [self button:UI("加入", "Add") action:@selector(addFile:)];
            b.tag = (NSInteger)self.addable.count; [self.addable addObject:add];
            b.enabled = !cannot;
            b.toolTip = cannot ?: UI("在终端里打包这个版本（大约 15 分钟）；完成后大厅会重新打开",
                                     "Builds this version in Terminal (about 15 minutes); the hub opens again when it is done");
            [views addObject:b];
        }
        NSStackView *row = [NSStackView stackViewWithViews:views];
        row.spacing = 8; row.translatesAutoresizingMaskIntoConstraints = NO;
        row.distribution = NSStackViewDistributionFill;   // the name takes the room, Add sits on the right
        [row setCustomSpacing:4 afterView:m];
        [self.files addArrangedSubview:row];
        [row.widthAnchor constraintEqualToAnchor:self.files.widthAnchor].active = YES;
        [row.heightAnchor constraintGreaterThanOrEqualToConstant:21].active = YES;
    };
    NSArray *names = [[fm contentsOfDirectoryAtPath:folder error:nil] sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
    NSInteger gameFiles = 0;
    for (NSString *name in names) {
        NSString *ext = name.pathExtension.lowercaseString;
        if (![@[@"xci", @"nsp", @"nsz", @"keys"] containsObject:ext]) continue;
        NSDictionary *attrs = [fm attributesOfItemAtPath:[folder stringByAppendingPathComponent:name] error:nil];
        if (![attrs.fileType isEqualToString:NSFileTypeRegular]) continue;
        if ([ext isEqualToString:@"keys"]) { line(@"✓", NSColor.systemGreenColor, UI("你的密钥", "Your keys"), name, nil); continue; }
        gameFiles++;
        NSDictionary *row = known[name];
        BOOL same = row && [row[@"size"] isKindOfClass:NSNumber.class] && [row[@"size"] unsignedLongLongValue] == attrs.fileSize &&
                    [row[@"mtime"] isKindOfClass:NSNumber.class] &&
                    fabs([row[@"mtime"] doubleValue] - attrs.fileModificationDate.timeIntervalSince1970) < 1;
        NSString *kind = same ? row[@"kind"] : nil, *version = same ? row[@"version"] : nil;
        if (([kind isEqual:@"base"] || [kind isEqual:@"update"] || [kind isEqual:@"dlc"]) && [version isKindOfClass:NSString.class]) {
            NSString *title = [row[@"title"] isKindOfClass:NSString.class] ? [row[@"title"] stringByAppendingString:@" "] : @"";
            NSString *what = [kind isEqual:@"base"] ? [NSString stringWithFormat:UI("%@%@ 本体", "%@%@ base game"), title, version]
                           : [kind isEqual:@"update"] ? [NSString stringWithFormat:UI("%@%@ 更新", "%@%@ update"), title, version]
                           : [NSString stringWithFormat:UI("%@%@ 追加内容 %@", "%@%@ DLC %@"), title, version, row[@"pack"]];
            NSString *game = [row[@"game"] isKindOfClass:NSString.class] ? row[@"game"] : @"";
            NSArray *adds = [row[@"adds"] isKindOfClass:NSArray.class] ? row[@"adds"] : @[version];   // the hub versions it is part of
            BOOL there = NO;
            for (NSString *v in adds) {
                if (![v isKindOfClass:NSString.class]) continue;
                if (!game.length) {   // a list from before "game"
                    there = there || [[self.grid.games valueForKey:@"version"] containsObject:v];
                    continue;
                }
                if (!found[game]) found[game] = [NSMutableSet set];
                [found[game] addObject:v];
                NSString *key = [NSString stringWithFormat:@"%@ %@", game, v];
                BOOL added = [inHub containsObject:[kind isEqual:@"dlc"] ? [key stringByAppendingFormat:@" dlc %@", row[@"pack"]] : key];
                there = there || added;
                if (!added && (!self.pendingFiles[key] || adds.count == 1)) self.pendingFiles[key] = name;   // a version's own file first
            }
            if (there) line(@"✓", NSColor.systemGreenColor, [what stringByAppendingString:UI(" · 已在大厅", " · in the hub")], name, nil);
            else line(@"•", NSColor.controlAccentColor, [what stringByAppendingString:UI(" · 还没加入", " · not added yet")], name, name);
        } else if ([kind isEqual:@"unsupported"]) {
            NSString *note = [row[@"note"] isKindOfClass:NSString.class] ? row[@"note"] : @"";
            line(@"✕", NSColor.secondaryLabelColor, [NSString stringWithFormat:UI("不使用：%@", "Not used: %@"), note], name, nil);
        } else line(@"•", NSColor.controlAccentColor, UI("新文件 · 加入时识别", "New · identified when you add it"), name, name);
    }
    if (!gameFiles)
        line(@"•", NSColor.secondaryLabelColor, UI("把游戏文件放在这里：本体（XCI 或 NSP）、更新（NSP 或 NSZ）、追加内容和 prod.keys。",
                                                   "Put your game files here: base games (XCI or NSP), updates (NSP or NSZ), DLC and prod.keys."), nil, nil);
    if (!keys) line(@"!", NSColor.systemOrangeColor, UI("这里没有 prod.keys：把你的密钥放进这个文件夹", "No prod.keys here: put your keys in this folder"), nil, nil);
    self.grid.canAdd = !cannot;
    [self.grid setGames:self.grid.games folder:found];
}

- (void)changeFolder:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES; panel.canChooseFiles = NO; panel.canCreateDirectories = YES;
    panel.prompt = UI("使用这个文件夹", "Use This Folder");
    panel.message = UI("选择放游戏文件和 prod.keys 的文件夹", "Choose the folder for your game files and prod.keys");
    panel.directoryURL = [NSURL fileURLWithPath:library_folder()];
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK || !panel.URL) return;
        library_save_folder(panel.URL.path);
        [self refreshLibrary];
    }];
}
- (void)showFolder:(id)sender {
    NSString *folder = library_folder();
    [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
    [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:folder]];
}
- (void)addFile:(NSButton *)sender {
    if (sender.tag >= 0 && sender.tag < (NSInteger)self.addable.count) [self addGame:self.addable[sender.tag]];
}
/* Runs build_app.py --hub --only <file> on the games folder in Terminal, then quits: the build adds that one version to
   this app (the others stay) and opens it again. One version per build: each takes about 15 minutes. */
- (void)addGame:(NSString *)file {
    NSAlert *ask = [NSAlert new];
    ask.messageText = UI("把这个版本加入大厅？", "Add this version to the hub?");
    ask.informativeText = [NSString stringWithFormat:UI("会在终端里打包 %@，大约 15 分钟。大厅先关闭，完成后自动重新打开。",
                                                        "This builds %@ in Terminal, about 15 minutes. The hub closes and opens again when it is done."), file];
    [ask addButtonWithTitle:UI("开始打包", "Build")];
    [ask addButtonWithTitle:UI("取消", "Cancel")];
    if ([ask runModal] != NSAlertFirstButtonReturn) return;
    NSDictionary *b = library_settings()[@"builder"];
    NSString *hub = NSBundle.mainBundle.bundlePath;
    NSMutableString *script = [NSMutableString stringWithString:@"#!/bin/zsh\n"];
    [script appendFormat:@"cd %@ || exit 1\n", shell_quote([b[@"script"] stringByDeletingLastPathComponent])];
    if ([b[@"developer_dir"] isKindOfClass:NSString.class] && [b[@"developer_dir"] length])
        [script appendFormat:@"export DEVELOPER_DIR=%@\n", shell_quote(b[@"developer_dir"])];
    [script appendFormat:@"%@ %@ --hub %@ --library %@ --only %@\n", shell_quote(b[@"python"]), shell_quote(b[@"script"]), shell_quote(hub),
        shell_quote(library_folder()), shell_quote(file)];   // the work folders: library.json "builder" too
    [script appendFormat:@"if [ $? -eq 0 ]; then echo; echo %@; else echo; echo %@; fi\nopen %@\n",
        shell_quote(UI("完成：大厅会重新打开。", "Done: the hub opens again.")),
        shell_quote(UI("打包停止了（原因见上面）；大厅保持原样重新打开。", "The build stopped (see above); the hub opens again as it was.")), shell_quote(hub)];
    NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"MacNativeGames - Add a game.command"];
    NSError *error = nil;
    if (![script writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:&error] ||
        ![NSFileManager.defaultManager setAttributes:@{NSFilePosixPermissions: @0755} ofItemAtPath:path error:&error]) {
        [[NSAlert alertWithError:error] runModal]; return;
    }
    NSURL *terminal = [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:@"com.apple.Terminal"];
    [NSWorkspace.sharedWorkspace openURLs:@[[NSURL fileURLWithPath:path]] withApplicationAtURL:terminal
                            configuration:[NSWorkspaceOpenConfiguration configuration]
                        completionHandler:^(NSRunningApplication *app, NSError *e) { (void)app;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (e) { [[NSAlert alertWithError:e] runModal]; return; }
            [NSApp terminate:nil];
        });
    }];
}

- (NSButton *)button:(NSString *)title action:(SEL)action {
    NSButton *b = [NSButton buttonWithTitle:title target:self action:action];
    b.translatesAutoresizingMaskIntoConstraints = NO;
    b.controlSize = NSControlSizeSmall; b.font = [NSFont systemFontOfSize:12];
    return b;
}
- (void)windowDidResize:(NSNotification *)note { [self.grid fit]; }

- (NSTextField *)label:(NSString *)text size:(CGFloat)size weight:(NSFontWeight)weight color:(NSColor *)color {
    NSTextField *t = [NSTextField labelWithString:text];
    t.font = [NSFont systemFontOfSize:size weight:weight]; t.textColor = color;
    t.translatesAutoresizingMaskIntoConstraints = NO;
    return t;
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    NSArray<HubGame *> *games = hub_games();
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1120, 860)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"MacNativeGames";
    self.window.minSize = NSMakeSize(CARD_W + 2 * MARGIN + 340, 830);   // a whole card above the game files
    self.window.delegate = self;
    self.window.frameAutosaveName = @"Hub";
    NSView *content = self.window.contentView;

    NSTextField *heading = [self label:UI("选择游戏", "Choose a game") size:26 weight:NSFontWeightBold color:NSColor.labelColor];
    NSString *hint = games.count ? UI("← → 选择游戏 · ↑ ↓ 或手柄 L / R 选择版本 · 回车或 A 键开始 · ⌘Q 退出",
                                      "← → choose a game · ↑ ↓ or L / R choose the version · Return or A to play · ⌘Q to quit")
                                 : UI("这个 App 里还没有游戏：把你的游戏文件和 prod.keys 放进下面的文件夹，再点游戏旁边的“加入”。",
                                      "No games in this app yet: put your game files and prod.keys in the folder below and click Add next to the game.");
    NSTextField *footer = [self label:hint size:12 weight:NSFontWeightRegular color:NSColor.secondaryLabelColor];
    footer.lineBreakMode = NSLineBreakByTruncatingTail;
    [footer setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSScrollView *scroll = [NSScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.drawsBackground = NO; scroll.hasHorizontalScroller = YES; scroll.autohidesScrollers = YES;
    self.grid = [[HubGrid alloc] initWithFrame:NSMakeRect(0, 0, 900, CARD_H)];
    scroll.documentView = self.grid;
    [self.grid setGames:games folder:@{}];
    __weak HubDelegate *weakSelf = self;
    self.grid.onAdd = ^(HubGame *g) {   // the card's "+" version: its file
        NSString *file = weakSelf.pendingFiles[[NSString stringWithFormat:@"%@ %@", g.key, g.version]];
        if (file) [weakSelf addGame:file]; else NSBeep();
    };

    NSBox *rule = [NSBox new]; rule.boxType = NSBoxSeparator; rule.translatesAutoresizingMaskIntoConstraints = NO;
    NSTextField *filesHeading = [self label:UI("游戏文件", "Game files") size:13 weight:NSFontWeightSemibold color:NSColor.labelColor];
    self.folderLabel = [self label:@"" size:12 weight:NSFontWeightRegular color:NSColor.secondaryLabelColor];
    self.folderLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [self.folderLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSButton *change = [self button:UI("更改…", "Change…") action:@selector(changeFolder:)];
    NSButton *show = [self button:UI("在访达中显示", "Show in Finder") action:@selector(showFolder:)];
    NSScrollView *filesScroll = [NSScrollView new];
    filesScroll.translatesAutoresizingMaskIntoConstraints = NO; filesScroll.drawsBackground = NO;
    filesScroll.hasVerticalScroller = YES; filesScroll.autohidesScrollers = YES;
    HubFlipped *filesDoc = [HubFlipped new];
    filesDoc.translatesAutoresizingMaskIntoConstraints = NO;
    filesScroll.documentView = filesDoc;
    self.files = [NSStackView new];
    self.files.orientation = NSUserInterfaceLayoutOrientationVertical;
    self.files.alignment = NSLayoutAttributeLeading; self.files.spacing = 2;
    self.files.translatesAutoresizingMaskIntoConstraints = NO;
    [filesDoc addSubview:self.files];
    [NSLayoutConstraint activateConstraints:@[
        [filesDoc.topAnchor constraintEqualToAnchor:filesScroll.contentView.topAnchor],
        [filesDoc.leadingAnchor constraintEqualToAnchor:filesScroll.contentView.leadingAnchor],
        [filesDoc.widthAnchor constraintEqualToAnchor:filesScroll.contentView.widthAnchor],
        [self.files.topAnchor constraintEqualToAnchor:filesDoc.topAnchor constant:4],
        [self.files.leadingAnchor constraintEqualToAnchor:filesDoc.leadingAnchor],
        [self.files.trailingAnchor constraintEqualToAnchor:filesDoc.trailingAnchor],
        [self.files.bottomAnchor constraintEqualToAnchor:filesDoc.bottomAnchor constant:-4],
    ]];

    for (NSView *v in @[heading, scroll, rule, filesHeading, self.folderLabel, change, show, filesScroll, footer])
        [content addSubview:v];
    [NSLayoutConstraint activateConstraints:@[
        [rule.topAnchor constraintEqualToAnchor:scroll.bottomAnchor constant:8],
        [rule.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:MARGIN],
        [rule.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-MARGIN],
        [filesHeading.topAnchor constraintEqualToAnchor:rule.bottomAnchor constant:12],
        [filesHeading.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:MARGIN],
        [self.folderLabel.firstBaselineAnchor constraintEqualToAnchor:filesHeading.firstBaselineAnchor],
        [self.folderLabel.leadingAnchor constraintEqualToAnchor:filesHeading.trailingAnchor constant:10],
        [change.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.folderLabel.trailingAnchor constant:10],
        [change.centerYAnchor constraintEqualToAnchor:filesHeading.centerYAnchor],
        [show.leadingAnchor constraintEqualToAnchor:change.trailingAnchor constant:6],
        [show.centerYAnchor constraintEqualToAnchor:filesHeading.centerYAnchor],
        [show.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-MARGIN],
        [filesScroll.topAnchor constraintEqualToAnchor:filesHeading.bottomAnchor constant:8],
        [filesScroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:MARGIN],
        [filesScroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-MARGIN],
        [filesScroll.heightAnchor constraintEqualToConstant:146],   // six files without scrolling
        [heading.topAnchor constraintEqualToAnchor:content.topAnchor constant:24],
        [heading.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:MARGIN],
        [scroll.topAnchor constraintEqualToAnchor:heading.bottomAnchor constant:16],
        [scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [footer.topAnchor constraintEqualToAnchor:filesScroll.bottomAnchor constant:10],
        [footer.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:MARGIN],
        [footer.trailingAnchor constraintLessThanOrEqualToAnchor:content.trailingAnchor constant:-MARGIN],
        [footer.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-16],
    ]];
    [self refreshLibrary];
    [content layoutSubtreeIfNeeded];
    [self.grid fit];
    if (![self.window setFrameUsingName:@"Hub"]) [self.window center];
    NSRect frame = self.window.frame, screen = self.window.screen.visibleFrame;   // a size saved by an earlier, smaller layout
    if (NSHeight(frame) < self.window.minSize.height || NSWidth(frame) < self.window.minSize.width) {
        CGFloat grow = MAX(0, self.window.minSize.height - NSHeight(frame));
        frame.origin.y -= grow; frame.size.height += grow;
        frame.size.width = MAX(NSWidth(frame), self.window.minSize.width);
        if (!NSIsEmptyRect(screen)) {   // kept on the screen
            frame.origin.y = MAX(NSMinY(screen), MIN(NSMinY(frame), NSMaxY(screen) - NSHeight(frame)));
            frame.origin.x = MAX(NSMinX(screen), MIN(NSMinX(frame), NSMaxX(screen) - NSWidth(frame)));
        }
        [self.window setFrame:frame display:NO];
    }
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.grid];
    [self.grid select:self.grid.selected];

    self.pad = [HubPad new];
    self.pad.grid = self.grid;
    [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 target:self.pad selector:@selector(poll) userInfo:nil repeats:YES];
    [NSApp activateIgnoringOtherApps:YES];
}
@end

static void hub_menu(void) {
    NSMenu *bar = [NSMenu new], *app = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new];
    appItem.submenu = app;
    [bar addItem:appItem];
    [app addItemWithTitle:UI("隐藏 MacNativeGames", "Hide MacNativeGames") action:@selector(hide:) keyEquivalent:@"h"];
    [app addItem:NSMenuItem.separatorItem];
    [app addItemWithTitle:UI("退出 MacNativeGames", "Quit MacNativeGames") action:@selector(terminate:) keyEquivalent:@"q"];
    NSApp.mainMenu = bar;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc > 1 && !strcmp(argv[1], "--check")) return hub_check();
        if (argc > 3 && !strcmp(argv[1], "--after")) return hub_after((pid_t)atoi(argv[2]), argv[3]);
        const char *direct = getenv("MNG_GAME");
        if (direct && *direct) {
            for (HubGame *g in hub_games())
                if ([g.ident isEqualToString:@(direct)]) { unsetenv("MNG_GAME"); [NSApplication sharedApplication]; hub_launch(g, NO); return 1; }
            fprintf(stderr, "[hub] no game %s in this app\n", direct);
        }
        NSApplication *app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        hub_menu();
        HubDelegate *delegate = [HubDelegate new];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}

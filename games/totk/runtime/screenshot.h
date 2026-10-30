/* The controller's capture button takes a picture of the game's own frame (a Nintendo controller's square button below the
   D-pad, macOS's Share button, and the joypad gamepad.m's HID fallback reads): the next frame the renderer presents is
   encoded off screen and saved, so the picture is the game picture -- not the boot screen's text, the FPS box or the
   settings panel, none of which the game draws. Where it goes depends on the platform:
     - iPhone and iPad: the system photo library (Photos), the way a console's Capture button does;
     - a desktop (macOS, Windows, Linux): a folder, the Desktop until the settings panel's folder chooser names another
       one; TOTK_SCREENSHOT_DIR names one for a launch.
   Until the game shows a picture the request saves nothing and the note says so. The folder, its checks, the name and the
   photo library's album name are plain C, so tests/test_screenshot.c covers them without a window.
   frontend.m includes this after settings_panel.h, so the note sits above the game's own views. */
#pragma once
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

/* The panel's texts follow the Mac's language (totk_ui_lang.h), which this header is included after. */
#ifndef TOTK_UI
#define TOTK_UI(zh, en) (en)
#endif

/* The album the pictures join in the photo library (a phone), and the file the settings panel keeps its folder in on a
   desktop, next to controls.txt (read only when the environment names no folder). */
#define TOTK_SCREENSHOT_ALBUM "Tears of the Kingdom"
#define TOTK_SCREENSHOT_SETTING "screenshot-dir.txt"
static char screenshot_folder_from_env[PATH_MAX];   /* a desktop's TOTK_SCREENSHOT_DIR, while it names one */

/* ---- The folder in force (a desktop) ----
   TOTK_SCREENSHOT_DIR first (a leading ~ is the home folder), then the panel's file (a relative path there is from the home
   folder), then the Desktop. out must hold PATH_MAX; NULL when the setting is too long for it. */
__attribute__((unused)) static const char *screenshot_folder(char *out, size_t size) {
    char text[PATH_MAX];
    const char *set = getenv("TOTK_SCREENSHOT_DIR");
    if (set && *set) snprintf(screenshot_folder_from_env, sizeof screenshot_folder_from_env, "%s", set);
    if (!(set && *set)) {
        FILE *file = fopen(TOTK_SCREENSHOT_SETTING, "r");
        if (file) {   /* the folder, without the blanks and newline around it */
            size_t n = fread(text, 1, sizeof text - 1, file);
            fclose(file);
            text[n] = 0;
            while (n && (text[n - 1] == '\n' || text[n - 1] == '\r' || text[n - 1] == ' ' || text[n - 1] == '\t')) text[--n] = 0;
            size_t skip = 0;
            while (text[skip] == ' ' || text[skip] == '\t') skip++;
            memmove(text, text + skip, n - skip + 1);
            set = text;
        }
    }
    if (set && *set) {
        const char *home = getenv("HOME");
        if (*set == '~' && (set[1] == '/' || !set[1])) {
            if (home && *home) { snprintf(out, size, "%s%s", home, set + 1); return out; }
        } else if (*set == '/') {
            snprintf(out, size, "%s", set);
            return out;
        }
        snprintf(out, size, "%s/%s", home && *home ? home : ".", set);   /* relative: from the home folder */
        return out;
    }
    const char *home = getenv("HOME");
    snprintf(out, size, "%s/Desktop", home && *home ? home : ".");
    return out;
}

/* The folder the settings panel shows, or "" when the setting is too long to resolve. */
__attribute__((unused)) static const char *screenshot_directory(char *out, size_t size) {
    if (!screenshot_folder(out, size)) snprintf(out, size, "%s", "");
    return out;
}

/* ---- Why a picture cannot be saved ----
   The ordinary ways a save fails are told apart, each with its own short reason: the folder went away (an external disk
   unplugged, the folder renamed or deleted), a file sits at the path, the folder is not writable (a read-only disk,
   another account's folder, a sandbox that says no), the path is impossible, there is no room; and on a phone, no
   permission for the photo library, or a frame too large to store. The checks run before the write, and the write's own
   failure is reported too, because a folder that looks well can still refuse. */
enum {
    SHOT_OK = 0,
    SHOT_NO_FOLDER,     /* the folder is gone */
    SHOT_NOT_FOLDER,    /* a file of that name is in the way */
    SHOT_NO_PERMISSION, /* not writable by this account, or the sandbox refuses it */
    SHOT_PATH_LONG,     /* the path does not fit */
    SHOT_FULL,          /* no room */
    SHOT_NO_PHOTOS,     /* a phone: the photo library was not allowed */
    SHOT_TOO_LARGE,     /* a picture too large to save */
};

/* The reason, one short line; "" for SHOT_OK. The folder's own path is added by the caller. */
static const char *screenshot_problem_text(int problem) {
    switch (problem) {
    case SHOT_OK: return "";
    case SHOT_NO_FOLDER: return TOTK_UI("目录不存在", "folder not found");
    case SHOT_NOT_FOLDER: return TOTK_UI("所选路径不是文件夹", "not a folder");
    case SHOT_NO_PERMISSION: return TOTK_UI("没有写入权限", "no write permission");
    case SHOT_PATH_LONG: return TOTK_UI("路径过长", "path too long");
    case SHOT_FULL: return TOTK_UI("空间不足", "not enough space");
    case SHOT_NO_PHOTOS: return TOTK_UI("没有访问照片的权限", "no access to Photos");
    case SHOT_TOO_LARGE: return TOTK_UI("画面尺寸超出上限", "picture too large");
    default: return TOTK_UI("未知错误", "unknown error");
    }
}

/* Can a picture be written into folder under name (NULL when only the folder is judged)? 1 and the free bytes, or 0 and
   which of SHOT_* says why. */
static int screenshot_problem(const char *folder, const char *name, int *problem, uint64_t *free_bytes) {
    struct stat st;
    *problem = SHOT_OK;
    if (free_bytes) *free_bytes = 0;
    if (!folder || !*folder) { *problem = SHOT_NO_FOLDER; return 0; }
    if (strlen(folder) >= PATH_MAX - 200 || (name && strlen(folder) + strlen(name) + 2 >= PATH_MAX)) { *problem = SHOT_PATH_LONG; return 0; }
    if (stat(folder, &st) != 0) {
        *problem = errno == ENOTDIR ? SHOT_NOT_FOLDER : errno == EACCES ? SHOT_NO_PERMISSION : SHOT_NO_FOLDER;
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) { *problem = SHOT_NOT_FOLDER; return 0; }
    if (access(folder, R_OK | X_OK | W_OK) != 0) { *problem = SHOT_NO_PERMISSION; return 0; }
    /* A folder this account owns but did not open to itself is refused: the mode bits are what the token then honours. An
       ACL or the sandbox is only found out by writing (screenshot_save reports that failure too). */
    if (st.st_uid == geteuid() && ((st.st_mode & 0700) != 0700)) { *problem = SHOT_NO_PERMISSION; return 0; }
    if (free_bytes) {
        struct statvfs vfs;
        *free_bytes = statvfs(folder, &vfs) == 0 ? (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize : 0;
        if (*free_bytes && *free_bytes < 256u * 1024u) { *problem = SHOT_FULL; return 0; }
    }
    return 1;
}

/* 1 with the folder kept, 0 with why not: *problem is the SHOT_* the folder fails (SHOT_OK when only the setting file
   could not be written), *free_bytes the room on it. The folder is checked with screenshot_problem first, so the chooser
   refuses a read-only disk or another account's folder while the player is looking at it, not at the next capture. An
   empty folder forgets the choice, back to the Desktop. */
__attribute__((unused)) static int screenshot_directory_set(const char *typed, const char **why, int *problem, uint64_t *free_bytes) {
    char folder[PATH_MAX] = {0};
    *problem = SHOT_OK;
    if (free_bytes) *free_bytes = 0;
    if (typed && *typed) {
        if (strlen(typed) >= sizeof folder) { *problem = SHOT_PATH_LONG; *why = screenshot_problem_text(*problem); return 0; }
        char room[PATH_MAX];
        const char *home = getenv("HOME");
        if (typed[0] == '~' && (typed[1] == '/' || !typed[1])) snprintf(room, sizeof room, "%s%s", home && *home ? home : "", typed + 1);
        else if (typed[0] == '/') snprintf(room, sizeof room, "%s", typed);
        else snprintf(room, sizeof room, "%s/%s", home && *home ? home : ".", typed);
        struct stat st;
        if (stat(room, &st) != 0) {
            /* Made when it is missing, its parents too (Finder's New Folder). The chooser makes the folder itself, so this
               is for a path from the environment or a script. */
            char parent[PATH_MAX];
            snprintf(parent, sizeof parent, "%s", room);
            for (char *slash = strrchr(parent, '/'); slash && slash != parent; slash = strrchr(parent, '/')) {
                *slash = 0;
                if (stat(parent, &st) == 0) break;
                if (mkdir(parent, 0755) != 0) break;
            }
            if (mkdir(room, 0755) != 0) {
                *problem = errno == ENOSPC ? SHOT_FULL : errno == EACCES || errno == EROFS ? SHOT_NO_PERMISSION : SHOT_NO_FOLDER;
                *why = screenshot_problem_text(*problem);
                return 0;
            }
        } else if (!S_ISDIR(st.st_mode)) { *problem = SHOT_NOT_FOLDER; *why = screenshot_problem_text(*problem); return 0; }
        if (!screenshot_problem(room, NULL, problem, free_bytes)) { *why = screenshot_problem_text(*problem); return 0; }
        snprintf(folder, sizeof folder, "%s", room);
    }
    if (!folder[0]) {   /* emptied: back to the Desktop */
        if (unlink(TOTK_SCREENSHOT_SETTING) != 0 && errno != ENOENT) { *why = TOTK_UI("无法删除设置", "setting not removed"); return 0; }
        return 1;
    }
    /* TOTK_SCREENSHOT_DIR decides before the file, so a launch with one would hide what the player just chose: this
       launch's environment then names the folder the file does, and both reads agree at once. */
    const char *now = getenv("TOTK_SCREENSHOT_DIR");   /* an empty one is no folder, and hides nothing */
    if (now && *now && strcmp(screenshot_folder_from_env, folder)) setenv("TOTK_SCREENSHOT_DIR", folder, 1);
    FILE *file = fopen(TOTK_SCREENSHOT_SETTING, "w");
    if (!file) { *why = TOTK_UI("无法保存设置", "setting not saved"); return 0; }
    int ok = fprintf(file, "%s\n", folder) > 0;
    if (fclose(file)) ok = 0;
    if (!ok) { *why = TOTK_UI("无法保存设置", "setting not saved"); unlink(TOTK_SCREENSHOT_SETTING); }
    return ok;
}

/* ---- The name ----
   <game> 2026-10-30 at 14.05.32.png, and (2), (3) ... when that second already has one. The photo library keeps the same
   stem for the album's copy, without the extension. */
__attribute__((unused)) static void screenshot_name(const char *game, const struct tm *local, char *out, size_t size) {
    snprintf(out, size, "%s %04d-%02d-%02d at %02d.%02d.%02d.png", game, local->tm_year + 1900, local->tm_mon + 1, local->tm_mday,
             local->tm_hour, local->tm_min, local->tm_sec);
}

/* <folder>/ followed by the first name of that second not taken yet; out must hold PATH_MAX, NULL when every name up to the
   999th is taken or the path does not fit. */
__attribute__((unused)) static char *screenshot_path(const char *folder, const char *game, const struct tm *local, char *out, size_t size) {
    int year = local->tm_year + 1900, month = local->tm_mon + 1, day = local->tm_mday;
    int hour = local->tm_hour, minute = local->tm_min, second = local->tm_sec;
    for (unsigned n = 0; n < 1000; n++) {
        int head = snprintf(out, size, "%s/%s %04d-%02d-%02d at %02d.%02d.%02d", folder, game, year, month, day, hour, minute, second);
        if (head < 0 || (size_t)head >= size) return NULL;
        int length = n == 0 ? snprintf(out + head, size - (size_t)head, ".png")
                            : snprintf(out + head, size - (size_t)head, " (%u).png", n + 1);
        if (length < 0 || (size_t)(head + length) >= size) return NULL;
        if (access(out, F_OK) != 0) return out;
    }
    return NULL;
}

/* ---- The notice over the game (macOS's window, the iPhone's view) ---- */
#if defined(TOTK_SCREENSHOT_WINDOW) || defined(TOTK_SCREENSHOT_VIEW)
#if defined(TOTK_SCREENSHOT_WINDOW)
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#else
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#endif

enum { SHOT_PENDING_MS = 3000, SHOT_NOTE_MS = 3000 };
/* The renderer's next frame: the runtime binary answers with one picture (frontend.m, mtl_grab_next_present; on iOS
   frontend_ios.m), a probe that links this file without the renderer with one of its own (TOTK_SCREENSHOT_PICTURE, defined
   before this header), or with none, which reads as no picture. Weak, so that a build without the renderer links. */
#ifndef TOTK_SCREENSHOT_PICTURE
#define TOTK_SCREENSHOT_PICTURE NULL
#endif
CGImageRef (*totk_screenshot_picture)(CGImageRef img, BOOL present) __attribute__((weak)) = TOTK_SCREENSHOT_PICTURE;
/* A program without ImageIO (a probe) or with another place to save (the iPhone app's photo library) defines
   TOTK_SCREENSHOT_WRITE 0 and offers its own screenshot_save. */
#ifndef TOTK_SCREENSHOT_WRITE
#define TOTK_SCREENSHOT_WRITE 1
#endif
/* What saves a picture: the folder below, or the program's own (a phone's photo library, a probe's). */
#ifndef TOTK_SCREENSHOT_SAVE
#define TOTK_SCREENSHOT_SAVE screenshot_save
#endif
static uint64_t shotPendingAt;   /* ns: a picture is on its way */
static dispatch_queue_t shotWrites;
#if !TOTK_SCREENSHOT_WRITE
/* A phone's save (frontend_ios.m: the photo library) and a probe's, in place of the folder below. */
static int TOTK_SCREENSHOT_SAVE(CGImageRef img, const char *game, char *path, size_t size, const char **why, int *problem,
                                char *album, size_t album_size);
#endif

static void screenshot_grab(void (^cb)(CGImageRef img)) {
    if (totk_screenshot_picture) { CGImageRef img = totk_screenshot_picture(NULL, NO); cb(img); CGImageRelease(img); }
    else cb(NULL);   /* no renderer here: said as no picture, which the note explains */
}

/* The line the player sees: what was saved and where, or why nothing was. Fades after SHOT_NOTE_MS, above the game's
   views. A failure wraps instead of being cut, since its reason names where the picture should have gone. */
#if defined(TOTK_SCREENSHOT_WINDOW)
static void screenshot_note(NSString *text, BOOL bad) {
    if (!window) return;
    NSTextField *line = [NSTextField labelWithString:text];
    line.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightMedium];
    line.lineBreakMode = bad ? NSLineBreakByWordWrapping : NSLineBreakByTruncatingMiddle;
    line.maximumNumberOfLines = bad ? 3 : 1;
    line.textColor = bad ? [NSColor colorWithCalibratedRed:1 green:0.45 blue:0.4 alpha:1]
                         : [NSColor colorWithCalibratedRed:80 / 255.0 green:1 blue:120 / 255.0 alpha:1];
    NSView *content = window.contentView;
    CGFloat room = fmax(160, content.bounds.size.width - 40);
    NSSize want = bad ? [line sizeThatFits:NSMakeSize(room - 28, CGFLOAT_MAX)] : [line sizeThatFits:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX)];
    CGFloat width = fmin(ceil(want.width) + 28, room), height = ceil(want.height) + 14;
    NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(floor((content.bounds.size.width - width) / 2), 24, width, height)];
    box.wantsLayer = YES; box.autoresizingMask = NSViewMinXMargin | NSViewMaxXMargin | NSViewMaxYMargin;
    box.layer.backgroundColor = [NSColor colorWithCalibratedWhite:0.04 alpha:0.78].CGColor;
    box.layer.cornerRadius = 8; box.layer.borderWidth = 1;
    box.layer.borderColor = [NSColor colorWithCalibratedWhite:1 alpha:0.14].CGColor;
    line.frame = NSMakeRect(14, 7, width - 28, ceil(want.height));
    [box addSubview:line]; [content addSubview:box];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)SHOT_NOTE_MS * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) { context.duration = 0.4; box.animator.alphaValue = 0; }
                            completionHandler:nil];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 450 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ [box removeFromSuperview]; });
    });
}
#else
/* The iPhone and iPad: a label near the bottom of the game view, inside the safe area. */
static void screenshot_note(NSString *text, BOOL bad) {
    UIView *host = bootOverlay.superview;
    if (!host) return;
    UILabel *line = [[UILabel alloc] initWithFrame:CGRectZero];
    line.text = text; line.numberOfLines = bad ? 3 : 1; line.textAlignment = NSTextAlignmentCenter;
    line.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightMedium];
    line.textColor = bad ? [UIColor colorWithRed:1 green:0.45 blue:0.4 alpha:1] : [UIColor colorWithRed:80 / 255.0 green:1 blue:120 / 255.0 alpha:1];
    UIEdgeInsets safe = host.safeAreaInsets;
    CGFloat room = MAX(160, host.bounds.size.width - 40 - safe.left - safe.right);
    CGSize want = [line sizeThatFits:CGSizeMake(room - 28, CGFLOAT_MAX)];
    CGFloat width = MIN(ceil(want.width) + 28, room), height = ceil(want.height) + 14;
    line.frame = CGRectMake(14, 7, width - 28, ceil(want.height));
    UIView *box = [[UIView alloc] initWithFrame:CGRectMake(floor((host.bounds.size.width - width) / 2), safe.bottom + 24, width, height)];
    box.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin | UIViewAutoresizingFlexibleTopMargin;
    box.backgroundColor = [UIColor colorWithWhite:0.04 alpha:0.78]; box.layer.cornerRadius = 8;
    box.layer.borderWidth = 1; box.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.14].CGColor;
    [box addSubview:line]; [host addSubview:box];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)SHOT_NOTE_MS * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        [UIView animateWithDuration:0.4 animations:^{ box.alpha = 0; } completion:^(BOOL finished) { (void)finished; [box removeFromSuperview]; }];
    });
}
#endif

#if defined(TOTK_SCREENSHOT_WINDOW)
/* Saves img and says where: a PNG in the screenshot folder. Returns 1 and the path written, or 0 and the reason. */
__attribute__((unused)) static int screenshot_save(CGImageRef img, const char *game, char *path, size_t size, const char **why, int *problem,
                           char *album, size_t album_size) {
    (void)album; (void)album_size;   /* a desktop has no album */
    char folder[PATH_MAX];
    const char *chosen = screenshot_folder(folder, sizeof folder);
    *problem = SHOT_PATH_LONG;
    *why = screenshot_problem_text(*problem);
    if (!chosen || !*chosen) return 0;
    if (!img) { *why = TOTK_UI("没有画面", "no picture"); return 0; }
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char name[NAME_MAX + 1];
    screenshot_name(game, &local, name, sizeof name);
    if (!screenshot_problem(folder, name, problem, NULL)) { *why = screenshot_problem_text(*problem); return 0; }
    if (!screenshot_path(folder, game, &local, path, size)) { *problem = SHOT_PATH_LONG; *why = screenshot_problem_text(*problem); return 0; }
    NSURL *url = [NSURL fileURLWithPath:@(path) isDirectory:NO];
    NSString *png = UTTypePNG ? UTTypePNG.identifier : nil;
    CGImageDestinationRef destination = png ? CGImageDestinationCreateWithURL((__bridge CFURLRef)url, (__bridge CFStringRef)png, 1, NULL) : NULL;
    if (!destination) {   /* the folder looked well and still refused: the sandbox, an ACL, a read-only volume ... */
        *problem = errno == ENOSPC ? SHOT_FULL : SHOT_NO_PERMISSION;
        *why = screenshot_problem_text(*problem);
        fprintf(stderr, "[totk] screenshot: opening %s for writing failed (errno %d)\n", path, errno);
        return 0;
    }
    CGImageDestinationAddImage(destination, img, NULL);
    int ok = CGImageDestinationFinalize(destination);
    CFRelease(destination);
    if (!ok) {
        *problem = errno == ENOSPC ? SHOT_FULL : SHOT_NO_PERMISSION;
        *why = screenshot_problem_text(*problem);
        fprintf(stderr, "[totk] screenshot: writing %s failed (errno %d)\n", path, errno);
        return 0;
    }
    return 1;
}
#endif

/* The request itself, on the main queue. */
static void screenshot_take(void) {
    uint64_t now = runtime_now_ns();
    if (shotPendingAt && now - shotPendingAt < (uint64_t)SHOT_PENDING_MS * 1000000ull) {
        fprintf(stderr, "[totk] screenshot: the last one is still on its way\n");
        return;
    }
    if (!totk_screenshot_picture || (bootOverlay && !bootOverlay.hidden)) {
        fprintf(stderr, "[totk] screenshot: the game has not shown a picture yet\n");
        screenshot_note(@(TOTK_UI("游戏画面尚未显示，未保存截图", "the game has not shown a picture yet; nothing saved")), YES);
        return;
    }
    if (!shotWrites) shotWrites = dispatch_queue_create("totk.screenshot", DISPATCH_QUEUE_SERIAL);
    shotPendingAt = now;
#if defined(TOTK_SCREENSHOT_WINDOW)
    char folder[PATH_MAX];
    screenshot_folder(folder, sizeof folder);
    NSString *place = @(folder);   /* the object the block captures: an array cannot cross into one */
    fprintf(stderr, "[totk] screenshot: taking the next frame into %s\n", folder);
#else
    NSString *place = @(TOTK_UI("照片", "Photos"));   /* the photo library */
#endif
    screenshot_grab(^(CGImageRef img) {
        char written[PATH_MAX] = {0};
        char album[NAME_MAX + 1] = {0};
        const char *why = NULL;
        int problem = SHOT_OK;
        int ok = TOTK_SCREENSHOT_SAVE(img, GAME_NAME, written, sizeof written, &why, &problem, album, sizeof album);
        NSString *text = ok
            ? (album[0] ? [NSString stringWithFormat:@(TOTK_UI("截图已存入「照片」：%@", "Screenshot saved to Photos: %@")), @(album)]
                        : [NSString stringWithFormat:@(TOTK_UI("截图已保存：%@", "Screenshot saved: %@")), @(written)])
            : [NSString stringWithFormat:@(TOTK_UI("未保存截图：%@（%@）", "Screenshot not saved: %@ (%@)")), @(why ?: "?"), place];
        dispatch_async(dispatch_get_main_queue(), ^{
            shotPendingAt = 0;
            fprintf(stderr, "[totk] screenshot: %s\n", text.UTF8String);
            screenshot_note(text, !ok);
        });
    });
}

/* gamepad.m's capture button (any queue): after the present in progress, so the request never holds the controller's
   report. */
static void screenshot_request(void) { dispatch_async(dispatch_get_main_queue(), ^{ screenshot_take(); }); }
#endif   /* TOTK_SCREENSHOT_WINDOW || TOTK_SCREENSHOT_VIEW */

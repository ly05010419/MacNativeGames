/* System functions Animal Crossing reaches in single-player play that would otherwise go to Switch services this host
   does not run (reported in play, PR #1305: "after I set up a tent, the game crashes whenever I click on the mailbox"; the log:
   sm: service "pctl" not implemented, from nn::pctl::CheckFreeCommunicationPermission). Each answers as a console with
   no restrictions and no network would:
     nn::pctl   parental controls: free communication is allowed (letters, the mailbox; nn::pctl::Initialize and the
                stereo-vision calls are in services.c, IsFreeCommunicationAvailable in applet_host.c)
     nn::err    the error applet: the error is logged, the game goes on as after the player closed the dialog
                (ShowError(Result) is applet_host.c's, as for 1.2.1)
     nn::ec     the eShop page (Happy Home Paradise): logged; nothing opens
     nn::account::LoadProfileImage  the local user's picture (account.c has the user and the rest of nn::account)
     nn::album  the in-game camera's photos: written as PNG to "Screenshots" next to the profile folder (the app's
                "Animal Crossing Data"); sharing has no service, so a shared photo is only saved
   Found by listing every SDK function main and subsdk0 import that no host override names (2026-10-29). */
#include "runtime.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---------- nn::pctl ---------- */
static uint32_t pctl_check_free_communication(bool show_ui) {
    static int once; if (!once++) fprintf(stderr, "[totk] parental controls: free communication allowed (local, ui %d)\n", show_ui);
    return 0;
}

/* ---------- nn::err ---------- */
typedef struct { uint32_t category, number; } ErrorCode;   /* nn::err::ErrorCode: 2xxx-yyyy */
static void err_show_code(ErrorCode code) { fprintf(stderr, "[totk] error applet: %04u-%04u (not shown; the game continues)\n", 2000 + code.category, code.number); }
static void err_show_variant(const void *variant) { (void)variant; fprintf(stderr, "[totk] error applet: an ErrorResultVariant (not shown; the game continues)\n"); }

/* ---------- nn::ec ---------- */
static uint32_t ec_show_shop(void) { fprintf(stderr, "[totk] eShop page requested: there is no eShop here; nothing opens\n"); return 0; }

/* ---------- nn::album ---------- */
enum { ALBUM_W = 1280, ALBUM_H = 720 };   /* nn::album::ImageSize_1280x720, RGBA8 */
static uint32_t album_initialize(void) { return 0; }
static void album_finalize(void) {}
static int album_folder(char *out, size_t size) {
    const char *profile = getenv("TOTK_PROFILE_DIR");
    if (!profile || !*profile) return 0;
    char parent[2048]; snprintf(parent, sizeof parent, "%s", profile);
    char *slash = strrchr(parent, '/'); if (slash) *slash = 0; else snprintf(parent, sizeof parent, ".");
    int n = snprintf(out, size, "%s/Screenshots", parent);
    if (n <= 0 || (size_t)n >= size || (mkdir(out, 0755) && access(out, W_OK))) return 0;
    return 1;
}
static uint32_t album_save(const void *image, size_t size, int image_size, int option) {
    (void)option;
    if (!image || image_size != 0 || size < (size_t)ALBUM_W * ALBUM_H * 4) {
        fprintf(stderr, "[totk] album: screenshot of size %zu (image size %d) not saved: not 1280x720 RGBA\n", size, image_size);
        return 0x1067;   /* nn::album: invalid image */
    }
    char dir[2100], path[2300];
    if (!album_folder(dir, sizeof dir)) { fprintf(stderr, "[totk] album: no folder for screenshots; not saved\n"); return 0x1067; }
    time_t now = time(NULL); struct tm t; localtime_r(&now, &t);
    static unsigned serial; snprintf(path, sizeof path, "%s/ACNH-%04d%02d%02d-%02d%02d%02d-%u.png", dir, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, serial++);
    CGColorSpaceRef rgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef data = CGDataProviderCreateWithData(NULL, image, (size_t)ALBUM_W * ALBUM_H * 4, NULL);
    CGImageRef picture = CGImageCreate(ALBUM_W, ALBUM_H, 8, 32, ALBUM_W * 4, rgb, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, data, NULL, false, kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef out = url ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    bool ok = false;
    if (out && picture) { CGImageDestinationAddImage(out, picture, NULL); ok = CGImageDestinationFinalize(out); }
    if (out) CFRelease(out);
    if (url) CFRelease(url);
    if (picture) CGImageRelease(picture);
    CGDataProviderRelease(data); CGColorSpaceRelease(rgb);
    fprintf(stderr, "[totk] album: screenshot %s %s\n", path, ok ? "saved" : "FAILED");
    return 0;
}
static uint32_t album_save_and_share(void *share_id, const void *image, size_t size, int image_size, int option, const void *uid, const char *text) {
    (void)uid; (void)text;
    if (share_id) memset(share_id, 0, 0x10);
    return album_save(image, size, image_size, option);   /* no sharing service: saved only */
}
static uint32_t album_last_url(char *out, size_t size, const void *share_id) { (void)share_id; if (out && size) out[0] = 0; return 0x1067; }

/* ---------- nn::account::LoadProfileImage ---------- */
/* The local user's profile picture (a 256x256 JPEG on a console; account.c has the user): a plain leaf-green picture,
   made once. */
static CFDataRef profile_image(void) {
    static CFDataRef jpeg; if (jpeg) return jpeg;
    CGColorSpaceRef rgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef c = CGBitmapContextCreate(NULL, 256, 256, 8, 256 * 4, rgb, kCGImageAlphaNoneSkipLast);
    CGContextSetRGBFillColor(c, 0.48, 0.78, 0.42, 1); CGContextFillRect(c, CGRectMake(0, 0, 256, 256));
    CGContextSetRGBFillColor(c, 1, 1, 1, 0.9); CGContextFillEllipseInRect(c, CGRectMake(68, 68, 120, 120));
    CGImageRef image = CGBitmapContextCreateImage(c); CFMutableDataRef data = CFDataCreateMutable(NULL, 0);
    CGImageDestinationRef out = CGImageDestinationCreateWithData(data, CFSTR("public.jpeg"), 1, NULL);
    CGImageDestinationAddImage(out, image, NULL); bool ok = CGImageDestinationFinalize(out);
    CFRelease(out); CGImageRelease(image); CGContextRelease(c); CGColorSpaceRelease(rgb);
    if (!ok) { CFRelease(data); return NULL; }
    return jpeg = data;
}
static uint32_t load_profile_image(size_t *size, void *out, size_t capacity, const uint64_t *uid) {
    uint64_t me[2]; runtime_account_uid(me);
    if (!size || !uid || uid[0] != me[0] || uid[1] != me[1]) return 0x7c67;
    CFDataRef jpeg = profile_image(); if (!jpeg) return 0x7c67;
    size_t n = (size_t)CFDataGetLength(jpeg); *size = n;
    if (out && capacity >= n) memcpy(out, CFDataGetBytePtr(jpeg), n);
    return out && capacity < n ? 0x7c67 : 0;
}

void *runtime_applets_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn4pctl32CheckFreeCommunicationPermissionEb", pctl_check_free_communication},
        {"_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", err_show_code},
        {"_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE", err_show_variant},
        {"_ZN2nn2ec30ShowShopApplicationInformationEv", ec_show_shop},
        {"_ZN2nn5album10InitializeEv", album_initialize},
        {"_ZN2nn5album8FinalizeEv", album_finalize},
        {"_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionE", album_save},
        {"_ZN2nn5album22SaveAndShareScreenshotEPNS0_7ShareIdEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionERKNS_7account3UidEPKc", album_save_and_share},
        {"_ZN2nn5album21GetSharePostedLastUrlEPcmRKNS0_7ShareIdE", album_last_url},
        {"_ZN2nn7account16LoadProfileImageEPmPvmRKNS0_3UidE", load_profile_image}};
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}

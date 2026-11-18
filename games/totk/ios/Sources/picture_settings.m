#import "picture_settings.h"
#include <stdio.h>

static NSUserDefaults *defaults(void) { return NSUserDefaults.standardUserDefaults; }

BOOL totk_has_custom_picture(void) { return [defaults() stringForKey:@"TOTK_CUSTOM_MODE"] != nil; }

pp_picture totk_current_picture(pp_profile *profile, pp_metalfx *mfx) {
    NSUserDefaults *d = defaults();
    pp_profile p = PP_PERFORMANCE; pp_metalfx m = PP_MFX_OFF;
    pp_profile_parse([d stringForKey:@"TOTK_PICTURE_PROFILE"].UTF8String, &p);
    pp_metalfx_parse([d stringForKey:@"TOTK_METALFX"].UTF8String, &m);
    pp_picture pic;
    pp_profile_picture(PP_PERFORMANCE, &pic);
    if (p != PP_CUSTOM) pp_profile_picture(p, &pic);
    else if (totk_has_custom_picture()) {
        pic.mode = [[d stringForKey:@"TOTK_CUSTOM_MODE"] isEqualToString:@"docked"] ? PP_DOCKED : PP_HANDHELD;
        unsigned w = 0;
        pic.scene_w = pp_scene_parse([d stringForKey:@"TOTK_CUSTOM_SCENE"].UTF8String, &w) ? w : 0;
        pic.taa = [d boolForKey:@"TOTK_CUSTOM_TAA"];
        pic.fps = [[d stringForKey:@"TOTK_CUSTOM_FPS"] isEqualToString:@"30"] ? 30 : 60;
        pic.switch_size = [d boolForKey:@"TOTK_CUSTOM_SWITCH_SIZE"];
        pic.aspect_169 = [[d stringForKey:@"TOTK_CUSTOM_ASPECT"] isEqualToString:@"0"];
        pic.present_60 = [d boolForKey:@"TOTK_CUSTOM_PRESENT_60"];
    }
    if (profile) *profile = p;
    if (mfx) *mfx = m;
    return pic;
}

void totk_choose_profile(pp_profile p) { [defaults() setObject:@(pp_profile_key(p)) forKey:@"TOTK_PICTURE_PROFILE"]; }
void totk_choose_metalfx(pp_metalfx m) { [defaults() setObject:@(pp_metalfx_key(m)) forKey:@"TOTK_METALFX"]; }

void totk_save_custom_picture(const pp_picture *p) {
    NSUserDefaults *d = defaults();
    [d setObject:p->mode == PP_DOCKED ? @"docked" : @"handheld" forKey:@"TOTK_CUSTOM_MODE"];
    [d setObject:@(pp_scene_key(p->scene_w)) forKey:@"TOTK_CUSTOM_SCENE"];
    [d setBool:p->taa != 0 forKey:@"TOTK_CUSTOM_TAA"];
    [d setObject:p->fps == 30 ? @"30" : @"60" forKey:@"TOTK_CUSTOM_FPS"];
    [d setBool:p->switch_size != 0 forKey:@"TOTK_CUSTOM_SWITCH_SIZE"];
    [d setObject:p->aspect_169 ? @"0" : @"" forKey:@"TOTK_CUSTOM_ASPECT"];
    [d setBool:p->present_60 != 0 forKey:@"TOTK_CUSTOM_PRESENT_60"];
    totk_choose_profile(PP_CUSTOM);
}

void totk_migrate_picture_rows(void) {
    NSUserDefaults *d = defaults();
    if ([d boolForKey:@"TOTK_PICTURE_MIGRATED"]) return;
    /* Only what this app stored (the persistent domain), not the pane's registered defaults nor a launch argument. */
    NSDictionary *stored = [d persistentDomainForName:NSBundle.mainBundle.bundleIdentifier] ?: @{};
    NSArray *old_keys = @[@"TOTK_DOCKED", @"TOTK_DOCKED_SCENE", @"TOTK_BOTW_SCENE", @"TOTK_PHONE_FPS", @"TOTK_CUSTOM_PICTURE",
                          @"TOTK_IOS_PICTURE_MODE", @"TOTK_RENDER_RES", @"TOTK_FPS_CAP", @"TOTK_MFX_UPSCALE", @"TOTK_TAA",
                          @"TOTK_ASPECT"];
    BOOL any = NO;
    for (NSString *key in old_keys) if (stored[key]) any = YES;
    if (any) {
        /* a number counts as its text: devicectl or an older build may have stored 30 as a number (after PR #2287) */
        NSString *(^text)(NSString *) = ^NSString *(NSString *key) {
            id v = stored[key];
            return [v isKindOfClass:NSString.class] ? v : [v isKindOfClass:NSNumber.class] ? [v stringValue] : nil;
        };
        pp_old old = {
            .docked = [stored[@"TOTK_DOCKED"] boolValue],
            .docked_scene = (text(@"TOTK_BOTW_SCENE") ?: text(@"TOTK_DOCKED_SCENE")).UTF8String,
            .phone_fps = text(@"TOTK_PHONE_FPS").UTF8String,
            .custom_picture = [stored[@"TOTK_CUSTOM_PICTURE"] boolValue],
            .picture_mode = text(@"TOTK_IOS_PICTURE_MODE").UTF8String,
            .render_res = text(@"TOTK_RENDER_RES").UTF8String,
            .fps_cap = text(@"TOTK_FPS_CAP").UTF8String,
            .mfx_upscale = [stored[@"TOTK_MFX_UPSCALE"] boolValue],
            .taa = [stored[@"TOTK_TAA"] boolValue],
            .aspect = text(@"TOTK_ASPECT").UTF8String,
#if !defined(GAME_BOTW) && !defined(TOTK_GAME_V143)
            .totk_old = 1,   /* 1.0 and 1.2.1: the handheld scene without a size was 1280x720 */
#endif
        };
        pp_picture pic; pp_metalfx mfx;
        pp_migrate(&old, PP_THIS_GAME, &pic, &mfx);
        totk_save_custom_picture(&pic);
        totk_choose_metalfx(mfx);
        fprintf(stderr, "[totk] picture: the old settings are now the custom profile (%s, scene %s, taa %d, %d fps, metalfx %s)\n",
                pic.mode == PP_DOCKED ? "docked" : "handheld", pp_scene_key(pic.scene_w)[0] ? pp_scene_key(pic.scene_w) : "automatic",
                pic.taa, pic.fps, pp_metalfx_key(mfx));
    }
    [d setBool:YES forKey:@"TOTK_PICTURE_MIGRATED"];
}

#define main packaged_launcher_main
#include "../games/totk/app/launcher.m"
#undef main
#include <assert.h>
/* The launcher's one save-sync call (app/webdav_sync.h) is outside this probe: its main never runs here. */
void mng_webdav_sync_profile(const char *game_folder, const char *profile, const char *slot_dir) {
    (void)game_folder; (void)profile; (void)slot_dir;
}
int main(void){@autoreleasepool{
 NSFileManager *fm=NSFileManager.defaultManager;NSString *folder=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
 [fm createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
 NSString *image=[folder stringByAppendingPathComponent:@"romfs.bin"],*mark=[folder stringByAppendingPathComponent:@"romfs.sha256"];
 assert(!game_data_in(folder,64,@"fixture"));
 [[NSMutableData dataWithLength:64] writeToFile:image atomically:YES];
 assert([game_data_in(folder,64,@"fixture") isEqualToString:folder]);assert(!game_data_in(folder,65,@"fixture"));
 [@"wrong" writeToFile:mark atomically:YES encoding:NSUTF8StringEncoding error:nil];assert(!game_data_in(folder,64,@"fixture"));
 [@"FIXTURE\n" writeToFile:mark atomically:YES encoding:NSUTF8StringEncoding error:nil];assert([game_data_in(folder,64,@"fixture") isEqualToString:folder]);
 [fm removeItemAtPath:folder error:nil];puts("pass: launcher size and image-pin checks");return 0;
}}

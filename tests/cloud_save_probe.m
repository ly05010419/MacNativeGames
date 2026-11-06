/* Execute the real folder-sync code against synthetic saves in a temporary directory. */
#include "../games/totk/runtime/cloud_save.m"
void runtime_boot_status(const char *stage,const char *detail){(void)stage;(void)detail;}
int runtime_fs_save_busy(void){return 0;}
int main(int argc,char **argv){
 @autoreleasepool {
  if(argc!=5)return 2;
  NSString *root=@(argv[1]), *profile=@(argv[2]), *uid=@(argv[3]), *mode=@(argv[4]);
  NSString *local=[[profile stringByAppendingPathComponent:@"save"] stringByAppendingPathComponent:uid];
  NSFileManager *fm=NSFileManager.defaultManager;
  [fm createDirectoryAtPath:local withIntermediateDirectories:YES attributes:nil error:nil];
  NSString *progress=[local stringByAppendingPathComponent:@"progress.sav"];
  [[mode isEqualToString:@"push"]?uid:@"local copy" writeToFile:progress atomically:YES encoding:NSUTF8StringEncoding error:nil];
  if(![mode isEqualToString:@"push"])[fm setAttributes:@{NSFileModificationDate:[NSDate dateWithTimeIntervalSince1970:1]} ofItemAtPath:progress error:nil];
  setenv("TOTK_CLOUD_SAVE_DIR",root.fileSystemRepresentation,1);
  runtime_cloud_save_start(local.fileSystemRepresentation,profile.fileSystemRepresentation);
  if([mode isEqualToString:@"played"])[@"played here" writeToFile:progress atomically:YES encoding:NSUTF8StringEncoding error:nil];   /* a save in the game */
  if([mode isEqualToString:@"push"] || [mode isEqualToString:@"mismatch"] || [mode isEqualToString:@"played"])push();
  fflush(NULL);
 }
 return 0;
}

#import <UIKit/UIKit.h>

/* Backup of Documents/save and Documents/game-slot into a folder the player picks (for example in the Files app): the
   copy outlives the app, because deleting it deletes Documents but not that folder. Off until Settings › Save backup
   › Automatic save backup is on (TOTK_BACKUP); the folder is then picked once and remembered, and the copy lands in
   <picked>/<game name>/{save,game-slot}.
   Import is the same group's one-shot switch, "Import a save on the next launch" (TOTK_SAVE_IMPORT): the next launch
   asks for a save folder and copies it in before the game thread starts, keeping the replaced save in
   Documents/save-before-import. See -prepareFrom:completion: and -importFrom:completion: in save_backup.m.
   The save sync between devices (runtime/cloud_save.m) keeps <picked>/Saves, so choosing one folder for both never has
   the two features write the same file. */
@interface TOTKSaveBackup : NSObject

+ (BOOL)enabled;
/* Resolves (or asks for) the backup folder, restores a fresh install, then calls ready once — and starts the game. */
+ (void)prepareFrom:(UIViewController *)host completion:(void (^)(void))ready;
+ (void)sync;

@end

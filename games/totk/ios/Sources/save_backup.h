#import <UIKit/UIKit.h>

/* iCloud Drive / Files backup of Documents/save and Documents/game-slot (see save_backup.m). Off unless the Settings
   switch sets TOTK_BACKUP=1; the folder is then picked once and remembered. */
@interface TOTKSaveBackup : NSObject

+ (BOOL)enabled;
/* Resolves (or asks for) the backup folder, restores a fresh install, then calls ready once — and starts the game. */
+ (void)prepareFrom:(UIViewController *)host completion:(void (^)(void))ready;
+ (void)sync;

@end

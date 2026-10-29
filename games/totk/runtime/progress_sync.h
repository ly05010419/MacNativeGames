#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN
extern NSString *const TOTKProgressDidChange;
extern NSString *const TOTKProgressDidCheck;
@interface TOTKProgressSync : NSObject
+ (instancetype)shared;
+ (void)showMenu:(nullable id)sender;
- (void)showButton:(nullable id)sender;
@property(atomic, readonly) NSDictionary *viewState;
@property(atomic, readonly) BOOL gameRunning;
/* A nil completion prepares the manager without starting the game. */
- (void)prepareProfile:(NSURL *)profile gameVersion:(NSString *)version host:(nullable id)host completion:(void (^ _Nullable)(void))completion;
- (void)showFrom:(nullable id)host;
- (void)chooseFolderFrom:(nullable id)host;
- (void)useFolder:(NSURL *)folder error:(NSError **)error;
- (void)refresh;
- (void)setSynchronizationEnabled:(BOOL)enabled;
- (void)cancelPendingSelection;
- (void)saveCommitted;
/* Called by fs commit while its exclusive save gate is held; never performs network I/O. */
- (void)sealCommittedGeneration:(uint64_t)generation;
- (void)selectVersion:(NSString *)version restore:(BOOL)restore;
/* nil selects the current complete version on the store queue, after an in-flight save finishes. */
- (void)nameBackup:(NSString *)name version:(nullable NSString *)version replacing:(nullable NSString *)backupID;
- (void)continueLocal;
@end
void totk_progress_save_committed(void);
void totk_progress_save_boundary(uint64_t generation);
/* Implemented by native platform UI, called on main only. */
void totk_progress_show_manager(TOTKProgressSync *sync, id _Nullable host);
void totk_progress_choose_folder(TOTKProgressSync *sync, id _Nullable host);
void totk_progress_offer_wait(TOTKProgressSync *sync, id _Nullable host);
/* iOS: standalone management navigation, or a dismissible sheet when enterGame is nil. */
id totk_progress_make_manager(TOTKProgressSync *sync, void (^ _Nullable enterGame)(void));
NS_ASSUME_NONNULL_END

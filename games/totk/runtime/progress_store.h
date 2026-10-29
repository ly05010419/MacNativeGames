#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/* Call on one serial queue. Only the caller's progress root and authorized cloud folder are writable. */
@interface TOTKProgressStore : NSObject
@property(nonatomic, readonly) NSURL *localRoot;
@property(nonatomic, readonly) NSString *deviceID;
@property(nonatomic, copy) NSString *deviceName;
@property(nonatomic, readonly) NSString *gameVersion;
@property(nonatomic, readonly, nullable) NSString *baseVersion;
@property(nonatomic, readonly, nullable) NSURL *workingProfile;
@property(nonatomic, readonly) NSDictionary<NSString *, NSDictionary *> *versions;
@property(nonatomic, readonly) NSArray<NSDictionary *> *backups;
@property(nonatomic, readonly) NSArray<NSDictionary *> *receipts;
@property(nonatomic, readonly) NSArray<NSString *> *heads;
@property(nonatomic, readonly) NSArray<NSString *> *pendingProblems;
- (nullable NSDictionary *)versionRecord:(NSString *)version error:(NSError **)error;
- (BOOL)ingestRecordAtURL:(NSURL *)file error:(NSError **)error;
- (BOOL)acknowledgeDownloads:(NSError **)error;

- (nullable instancetype)initWithRoot:(NSURL *)root device:(NSString *)device gameVersion:(NSString *)version error:(NSError **)error;
/* Capture account.uid + save/<identity>/ only. Caller must freeze game save writes for the whole capture. */
- (nullable NSString *)captureProfile:(NSURL *)profile error:(NSError **)error;
/* Imports are union-only. A malformed/missing record is pending, never permission to remove local history. */
- (BOOL)exchangeWithFolder:(NSURL *)authorizedFolder error:(NSError **)error;
/* Must be called before the game opens the account/save mounts. Original profiles are never replaced. */
- (BOOL)checkout:(NSString *)version resolving:(BOOL)resolve error:(NSError **)error;
- (nullable NSString *)nameBackup:(NSString *)name forVersion:(NSString *)version replacing:(nullable NSString *)backupID error:(NSError **)error;
- (nullable NSString *)restoreVersion:(NSString *)version error:(NSError **)error;
- (BOOL)isDescendant:(NSString *)candidate of:(NSString *)ancestor;
/* Detect an interrupted/failed capture without blessing mutable working files as a complete version. */
- (BOOL)workingProfileMatchesBase:(NSError **)error;
- (BOOL)canLoadVersion:(NSString *)version error:(NSError **)error;
@end

NS_ASSUME_NONNULL_END

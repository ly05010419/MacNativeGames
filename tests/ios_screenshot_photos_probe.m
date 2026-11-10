/* Execute frontend_ios.m's Photos path against a permission-enforcing PhotoKit double. */
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#include <limits.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef const void *CGImageRef;
static size_t CGImageGetWidth(CGImageRef p) { (void)p; return 4; }
static size_t CGImageGetHeight(CGImageRef p) { (void)p; return 4; }
#define TOTK_UI(zh,en) en
#define TOTK_SCREENSHOT_ALBUM "Test Game"
enum { SHOT_OK, SHOT_TOO_LARGE, SHOT_NO_PHOTOS };
static const char *screenshot_problem_text(int p) { (void)p; return "permission denied"; }
static void screenshot_name(const char *g, const struct tm *t, char *n, size_t z) { (void)t; snprintf(n,z,"%s.png",g); }
typedef NS_ENUM(NSInteger, PHAuthorizationStatus) { PHAuthorizationStatusNotDetermined, PHAuthorizationStatusRestricted, PHAuthorizationStatusDenied, PHAuthorizationStatusAuthorized, PHAuthorizationStatusLimited };
typedef NS_ENUM(NSInteger, PHAccessLevel) { PHAccessLevelAddOnly, PHAccessLevelReadWrite };
enum { PHAssetCollectionTypeAlbum, PHAssetCollectionSubtypeAny };
static PHAuthorizationStatus readStatus, addStatus;
static int reads, writes, albums, requests, failAlbum;
@interface UIImage : NSObject
+ (instancetype)imageWithCGImage:(CGImageRef)p;
@end
@implementation UIImage
+ (instancetype)imageWithCGImage:(CGImageRef)p { return p ? [self new] : nil; }
@end
@interface PHObjectPlaceholder : NSObject @end
@implementation PHObjectPlaceholder @end
@interface PHFetchResult<ObjectType> : NSArray<ObjectType> @end
@interface PHAssetCollection : NSObject
@property NSString *localizedTitle;
+ (PHFetchResult *)fetchAssetCollectionsWithType:(NSInteger)t subtype:(NSInteger)s options:(id)o;
@end
@implementation PHAssetCollection
+ (PHFetchResult *)fetchAssetCollectionsWithType:(NSInteger)t subtype:(NSInteger)s options:(id)o {
 (void)t;(void)s;(void)o; reads++; assert(readStatus==PHAuthorizationStatusAuthorized); return (id)@[];
}
@end
@interface PHAssetChangeRequest : NSObject
@property PHObjectPlaceholder *placeholderForCreatedAsset;
+ (instancetype)creationRequestForAssetFromImage:(UIImage *)p;
@end
@implementation PHAssetChangeRequest
+ (instancetype)creationRequestForAssetFromImage:(UIImage *)p { assert(p); writes++; PHAssetChangeRequest *r=[self new]; r.placeholderForCreatedAsset=[PHObjectPlaceholder new]; return r; }
@end
@interface PHAssetCollectionChangeRequest : NSObject
+ (instancetype)changeRequestForAssetCollection:(PHAssetCollection *)c;
+ (instancetype)creationRequestForAssetCollectionWithTitle:(NSString *)t;
- (void)addAssets:(id)a;
@end
@implementation PHAssetCollectionChangeRequest
+ (instancetype)changeRequestForAssetCollection:(PHAssetCollection *)c { (void)c; return [self new]; }
+ (instancetype)creationRequestForAssetCollectionWithTitle:(NSString *)t { assert([t isEqualToString:@TOTK_SCREENSHOT_ALBUM]); albums++; return [self new]; }
- (void)addAssets:(id)a { (void)a; }
@end
@interface PHPhotoLibrary : NSObject
+ (PHAuthorizationStatus)authorizationStatusForAccessLevel:(PHAccessLevel)l;
+ (void)requestAuthorizationForAccessLevel:(PHAccessLevel)l handler:(void (^)(PHAuthorizationStatus))h;
+ (instancetype)sharedPhotoLibrary;
- (BOOL)performChangesAndWait:(void (^)(void))b error:(NSError **)e;
@end
@implementation PHPhotoLibrary
+ (PHAuthorizationStatus)authorizationStatusForAccessLevel:(PHAccessLevel)l { return l==PHAccessLevelReadWrite?readStatus:addStatus; }
+ (void)requestAuthorizationForAccessLevel:(PHAccessLevel)l handler:(void (^)(PHAuthorizationStatus))h { requests++; assert(l==PHAccessLevelReadWrite); readStatus=PHAuthorizationStatusAuthorized; h(readStatus); }
+ (instancetype)sharedPhotoLibrary { return [self new]; }
- (BOOL)performChangesAndWait:(void (^)(void))b error:(NSError **)e {
 if(failAlbum){failAlbum=0; *e=[NSError errorWithDomain:@"album" code:1 userInfo:nil]; return NO;}
 b(); return YES;
}
@end
#include "photos-production.inc"
int main(int argc,char **argv) { @autoreleasepool {
 assert(argc==2); const char *mode=argv[1];
 readStatus=PHAuthorizationStatusAuthorized;addStatus=PHAuthorizationStatusDenied;
 if(!strcmp(mode,"limited"))readStatus=PHAuthorizationStatusLimited;
 if(!strcmp(mode,"add-only")){readStatus=PHAuthorizationStatusDenied;addStatus=PHAuthorizationStatusAuthorized;}
 if(!strcmp(mode,"denied"))readStatus=PHAuthorizationStatusDenied;
 if(!strcmp(mode,"request"))readStatus=PHAuthorizationStatusNotDetermined;
 if(!strcmp(mode,"album-failure"))failAlbum=1;
 const char *why=NULL;int problem=-1;char path[64]={0},album[64]={0};
 int ok=screenshot_save((CGImageRef)1,"Test Game",path,sizeof path,&why,&problem,album,sizeof album);
 if(!strcmp(mode,"denied")){assert(!ok && problem==SHOT_NO_PHOTOS && reads==0 && writes==0);}
 else {assert(ok && writes==1);if(!strcmp(mode,"limited")||!strcmp(mode,"add-only")||!strcmp(mode,"album-failure")){assert(reads==0 && albums==0);assert(!strcmp(album,"Photo Library"));}else{assert(reads==1 && albums==1);assert(!strcmp(album,TOTK_SCREENSHOT_ALBUM));}}
 if(!strcmp(mode,"request"))assert(requests==1);
 puts("PASS: screenshot permissions and destination");
 } return 0; }

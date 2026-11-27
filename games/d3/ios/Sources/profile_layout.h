/* Only Documents/profile is used. Older save locations are never inspected or moved. */
#import <Foundation/Foundation.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>

static NSString *ios_profile_prepare(NSString *root) {
 NSString *profile=[root stringByAppendingPathComponent:@"profile"];
 struct stat st;
 if(!lstat(profile.fileSystemRepresentation,&st))
  return S_ISDIR(st.st_mode)?nil:@"Profile folder is not a directory.";
 if(errno!=ENOENT)return @"Cannot inspect Documents/profile.";
 if(![NSFileManager.defaultManager createDirectoryAtPath:profile withIntermediateDirectories:NO attributes:nil error:nil])
  return @"Could not create Documents/profile.";
 return nil;
}

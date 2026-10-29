#include "../third_party/d2r_gfx/gfx/nvn/include/totk_ui_lang.h"
#pragma once
#import <Foundation/Foundation.h>
#import <FileProvider/NSFileProviderError.h>
#include <errno.h>

static inline NSString * _Nullable TOTKProgressErrorMessage(NSError * _Nullable error) {
    if(!error)return nil;
    NSUInteger depth=0;
    for(NSError *item=error, *next=nil;item&&depth++<8;item=next) {
        BOOL cocoa=[item.domain isEqual:NSCocoaErrorDomain],posix=[item.domain isEqual:NSPOSIXErrorDomain];
        BOOL provider=[item.domain isEqual:@"NSFileProviderErrorDomain"];
        if((cocoa&&item.code==NSUbiquitousFileNotUploadedDueToQuotaError)||(provider&&item.code==NSFileProviderErrorInsufficientQuota))
            return @(TOTK_UI("iCloud 空间不足。本机进度已保留，请释放云空间后检查同步。", "iCloud storage is full. Local progress is retained. Free cloud space and check sync again."));
        if((cocoa&&item.code==NSFileWriteOutOfSpaceError)||(posix&&item.code==ENOSPC))
            return @(TOTK_UI("存储空间不足，已有进度已保留。请释放空间后重试。", "Storage is full. Existing progress is retained. Free space and try again."));
        if((provider&&item.code==NSFileProviderErrorNotAuthenticated)||(cocoa&&(item.code==NSFileReadNoPermissionError||item.code==NSFileWriteNoPermissionError))||(posix&&(item.code==EACCES||item.code==EPERM||item.code==EROFS)))
            return @(TOTK_UI("无法访问进度文件夹，已有进度已保留。请确认云服务已登录，或重新选择可读写的同步文件夹。", "The progress folder is inaccessible. Existing progress is retained. Sign in to your cloud service or choose a writable sync folder again."));
        if([item.domain isEqual:NSURLErrorDomain]||(provider&&item.code==NSFileProviderErrorServerUnreachable)||(cocoa&&item.code==NSUbiquitousFileUbiquityServerNotAvailable))
            return @(TOTK_UI("云文件服务暂不可用，本机进度已保留。网络恢复后会重试，也可以稍后检查同步。", "The cloud file service is unavailable. Local progress is retained. Sync retries when the network returns; you can also check again later."));
        if((cocoa&&(item.code==NSFileReadNoSuchFileError||item.code==NSFileNoSuchFileError))||(posix&&item.code==ENOENT)||(provider&&item.code==NSFileProviderErrorNoSuchItem))
            return @(TOTK_UI("进度文件暂不可用，请稍后检查同步；若文件夹已移动，请重新选择。已有进度已保留。", "Progress files are unavailable. Check sync later, or choose the folder again if it was moved. Existing progress is retained."));
        next=item.userInfo[NSUnderlyingErrorKey];
        if(next==item||![next isKindOfClass:NSError.class])break;
    }
    return error.localizedDescription;
}

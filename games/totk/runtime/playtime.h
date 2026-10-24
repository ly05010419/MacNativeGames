/* Per-profile foreground playtime. AppKit's main thread owns this counter.
   A monotonic clock is supplied by the caller; long timer gaps are not playtime.
   Atomic checkpoints bound crash loss to 15 seconds. Existing metadata survives. */
#pragma once
#import <Foundation/Foundation.h>
#include "../third_party/d2r_gfx/gfx/nvn/include/totk_ui_lang.h"
#include <math.h>
@interface TOTKPlaytime : NSObject
@property double seconds, lastTick, savedSeconds;
@property BOOL active, hasTick, writable;
@property(copy) NSString *path;
@property(strong) NSMutableDictionary *record;
-(instancetype)initWithPath:(NSString *)path;
-(void)tick:(double)now active:(BOOL)active;
-(BOOL)save;
-(NSString *)text;
@end
@implementation TOTKPlaytime
-(instancetype)initWithPath:(NSString *)path {
 if((self=[super init])) {
  self.path=path;self.writable=YES;self.record=[NSMutableDictionary new];
  if([NSFileManager.defaultManager fileExistsAtPath:path]) {
   NSData *data=[NSData dataWithContentsOfFile:path];
   id obj=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
   id value=[obj isKindOfClass:NSDictionary.class]?obj[@"seconds"]:nil;
   if(![value isKindOfClass:NSNumber.class]||!isfinite([value doubleValue])||[value doubleValue]<0||[value doubleValue]>1e12) {
    self.writable=NO;fprintf(stderr,"[totk] playtime: invalid file, left untouched: %s\n",path.UTF8String);
   } else {self.record=[obj mutableCopy];self.seconds=[value doubleValue];}
  }
  self.savedSeconds=self.seconds;
 }
 return self;
}
-(void)tick:(double)now active:(BOOL)active {
 double dt=now-self.lastTick;
 if(self.writable&&self.hasTick&&self.active&&dt>0&&dt<=5)self.seconds+=dt;
 BOOL stopped=self.active&&!active;
 self.lastTick=now;self.hasTick=YES;self.active=active;
 if(stopped||self.seconds-self.savedSeconds>=15)[self save];
}
-(BOOL)save {
 if(!self.writable)return NO;
 if(self.seconds==self.savedSeconds)return YES;
 self.record[@"schema"]=@1;self.record[@"seconds"]=@(self.seconds);
 NSError *error=nil;
 NSData *data=[NSJSONSerialization dataWithJSONObject:self.record options:NSJSONWritingPrettyPrinted error:&error];
 NSString *parent=self.path.stringByDeletingLastPathComponent;
 BOOL ok=data&&[NSFileManager.defaultManager createDirectoryAtPath:parent withIntermediateDirectories:YES attributes:nil error:&error]
  &&[data writeToFile:self.path options:NSDataWritingAtomic error:&error];
 if(ok)self.savedSeconds=self.seconds;
 else fprintf(stderr,"[totk] playtime save failed: %s\n",error.localizedDescription.UTF8String);
 return ok;
}
-(NSString *)text {
 if(!self.writable)return @(TOTK_UI("游玩时间不可用","Playtime unavailable"));
 unsigned long long minutes=(unsigned long long)(self.seconds/60);
 return [NSString stringWithFormat:@(TOTK_UI("游玩时间  %llu 小时 %02llu 分","Playtime  %llu h %02llu m")),minutes/60,minutes%60];
}
@end

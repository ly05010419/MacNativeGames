/* Shared Foundation model for Mac/iOS offline users. No save payload writes.
   Registry edits are locked and atomically published. Deleted saves are archived;
   new users reuse the smallest available ID. Store and runtime account_users.h use the same UTF-8 TSV format. */
#import <Foundation/Foundation.h>
#include "user_avatar.h"
#include "user_lock.h"
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

@interface MNGUserStore : NSObject
@property(nonatomic,copy) NSString *root;
@property(nonatomic) int reservationFD;
@property(nonatomic,strong) NSMutableDictionary<NSNumber *,NSString *> *users;
@property(nonatomic) NSUInteger selected;
@property(nonatomic,strong) NSMutableDictionary<NSString *,NSString *> *avatars;
- (NSString *)avatarForUser:(NSUInteger)uid;
- (instancetype)initWithRoot:(NSString *)root;
- (BOOL)reload:(NSString **)error;
- (BOOL)edit:(NSString *)operation user:(NSUInteger)uid name:(NSString *)name error:(NSString **)error;
- (NSArray<NSNumber *> *)ids;
- (NSString *)title:(NSNumber *)uid;
@end
@implementation MNGUserStore
- (instancetype)initWithRoot:(NSString *)root { if((self=[super init])){_root=[root copy];_reservationFD=-1;}return self; }
- (void)dealloc { if(_reservationFD>=0)close(_reservationFD); }
- (NSString *)avatarForUser:(NSUInteger)uid { return self.avatars[[@(uid) stringValue]] ?: mng_user_avatar_choices().firstObject; }
- (NSArray<NSNumber *> *)ids { return [self.users.allKeys sortedArrayUsingSelector:@selector(compare:)]; }
- (NSString *)title:(NSNumber *)uid { return [NSString stringWithFormat:@"%02lu — %@",(unsigned long)uid.unsignedIntegerValue,self.users[uid]]; }
- (BOOL)loadLocked:(NSString **)error {
 self.users=[NSMutableDictionary dictionary];self.selected=1;
 NSString *path=[self.root stringByAppendingPathComponent:@"users.tsv"];
 struct stat st;int rc=lstat(path.fileSystemRepresentation,&st);
 if(rc&&errno!=ENOENT){*error=@"无法读取用户列表 / Cannot read user list";return NO;}
 if(!rc){
  if(!S_ISREG(st.st_mode)||st.st_size<1||st.st_size>4096){*error=@"用户列表损坏 / Invalid user list";return NO;}
  NSString *text=[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
  NSArray *lines=[text componentsSeparatedByString:@"\n"];
  if(!text||![text hasSuffix:@"\n"]||lines.count<3){*error=@"用户列表损坏 / Invalid user list";return NO;}
  for(NSUInteger i=0;i+1<lines.count;i++){
   NSArray *parts=[lines[i] componentsSeparatedByString:@"\t"];
   if(parts.count!=2){*error=@"用户列表损坏 / Invalid user row";return NO;}
   NSString *number=i?parts[0]:parts[1];NSScanner *scan=[NSScanner scannerWithString:number];unsigned long long uid=0;
   if(![scan scanUnsignedLongLong:&uid]||!scan.isAtEnd||!uid||uid>32){*error=@"用户编号无效 / Invalid user ID";return NO;}
   if(!i){if(![parts[0] isEqual:@"selected"]){*error=@"用户列表损坏";return NO;}self.selected=(NSUInteger)uid;}
   else {NSString *name=parts[1];if(self.users[@(uid)]||!name.length||[name lengthOfBytesUsingEncoding:NSUTF8StringEncoding]>32||[name rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location!=NSNotFound){*error=@"用户名称无效 / Invalid user name";return NO;}self.users[@(uid)]=name;}
  }
  if(!self.users.count||!self.users[@(self.selected)]){*error=@"找不到已选用户 / Selected user missing";return NO;}
 }else{
  self.users[@1]=@"用户 01";

 }
 // UI-only metadata, deliberately separate from the runtime user registry.
 self.avatars=[NSMutableDictionary dictionary];
 NSString *avatarsPath=[self.root stringByAppendingPathComponent:@"avatars.plist"];
 struct stat avatarStat;int avatarRC=lstat(avatarsPath.fileSystemRepresentation,&avatarStat);
 if(avatarRC&&errno!=ENOENT){*error=@"无法读取头像 / Cannot read avatars";return NO;}
 if(!avatarRC){
  if(!S_ISREG(avatarStat.st_mode)||avatarStat.st_size>16384){*error=@"头像设置无效 / Invalid avatars";return NO;}
  NSDictionary *saved=[NSDictionary dictionaryWithContentsOfFile:avatarsPath];
  if(!saved){*error=@"头像设置无效 / Invalid avatars";return NO;}
  for(id key in saved){
   if(![key isKindOfClass:NSString.class]||![saved[key] isKindOfClass:NSString.class]||
      ![mng_user_avatar_choices() containsObject:saved[key]]||[key integerValue]<1||[key integerValue]>32||
      ![key isEqualToString:[@([key integerValue]) stringValue]]){
    *error=@"头像设置无效 / Invalid avatars";return NO;
   }
   if(self.users[@([key integerValue])])self.avatars[key]=saved[key];
  }
 }
 return YES;
}
- (BOOL)transaction:(NSString *)operation user:(NSUInteger)uid name:(NSString *)name error:(NSString **)error {
 int fd=mng_registry_lock(self.root.fileSystemRepresentation);
 if(fd<0){*error=@"无法锁定用户列表 / Cannot lock user list";return NO;}
 BOOL ok=[self loadLocked:error];
 int userLock=-1;
 if(ok&&([operation isEqual:@"delete"]||[operation isEqual:@"reserve"])){
  userLock=mng_user_lock(self.root.fileSystemRepresentation,(unsigned)uid);
  if(userLock<0){*error=@"用户正在使用或无法锁定 / User is in use or cannot be locked";ok=NO;}
 }
 NSString *movedFrom=nil,*movedTo=nil;
 if(ok&&operation){
  if([operation isEqual:@"add"]){
   for(uid=1;uid<=32&&self.users[@(uid)];uid++){}
   if(uid>32){*error=@"最多支持32个用户 / Maximum 32 users";ok=NO;}
   else {
    NSString *save=[self.root stringByAppendingPathComponent:[NSString stringWithFormat:@"save/%032lx",(unsigned long)uid]];
    struct stat existing;
    if(!lstat(save.fileSystemRepresentation,&existing)||errno!=ENOENT){
     *error=@"该空闲ID仍有存档目录，请手动移出后再新增，避免继承旧进度。 / Save directory already exists for this free ID.";ok=NO;
    }else{self.users[@(uid)]=[NSString stringWithFormat:@"用户 %02lu",(unsigned long)uid];self.selected=uid;[self.avatars removeObjectForKey:[@(uid) stringValue]];}
   }
  }
  else if(!self.users[@(uid)]){*error=@"用户不存在 / User not found";ok=NO;}
  else if([operation isEqual:@"delete"]){
   if(self.users.count<=1){*error=@"至少保留一位用户 / Keep at least one user";ok=NO;}
   else {
    NSString *saveRoot=[self.root stringByAppendingPathComponent:@"save"];
    NSString *source=[saveRoot stringByAppendingPathComponent:[NSString stringWithFormat:@"%032lx",(unsigned long)uid]];
    struct stat st;
    int rc=lstat(saveRoot.fileSystemRepresentation,&st);
    if((!rc&&!S_ISDIR(st.st_mode))||(rc&&errno!=ENOENT)){*error=@"存档根目录无效 / Invalid save directory";ok=NO;}
    if(ok){
     rc=lstat(source.fileSystemRepresentation,&st);
     if((!rc&&!S_ISDIR(st.st_mode))||(rc&&errno!=ENOENT)){*error=@"用户存档目录无效 / Invalid user save directory";ok=NO;}
     else if(!rc){
      NSString *archive=[self.root stringByAppendingPathComponent:@"deleted-users"];
      if(mkdir(archive.fileSystemRepresentation,0700)&&errno!=EEXIST){*error=@"无法创建存档归档 / Cannot create archive";ok=NO;}
      else if(lstat(archive.fileSystemRepresentation,&st)||!S_ISDIR(st.st_mode)){*error=@"归档目录无效 / Invalid archive directory";ok=NO;}
      else{
       NSString *destination=[archive stringByAppendingPathComponent:[NSString stringWithFormat:@"%02lu-%@",(unsigned long)uid,NSUUID.UUID.UUIDString]];
       if(renamex_np(source.fileSystemRepresentation,destination.fileSystemRepresentation,RENAME_EXCL)){*error=@"无法归档存档，未删除用户 / Could not archive save";ok=NO;}
       else{movedFrom=source;movedTo=destination;}
      }
     }
    }
    if(ok){[self.users removeObjectForKey:@(uid)];[self.avatars removeObjectForKey:[@(uid) stringValue]];
     if(self.selected==uid)self.selected=self.ids.firstObject.unsignedIntegerValue;}
   }
  }
  else if([operation isEqual:@"avatar"]){
   if(![mng_user_avatar_choices() containsObject:name]){*error=@"无效头像 / Invalid avatar";ok=NO;}
   else self.avatars[[@(uid) stringValue]]=name;
  }
  else if([operation isEqual:@"rename"]){
   NSString *trim=[name stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
   if(!trim.length||[trim lengthOfBytesUsingEncoding:NSUTF8StringEncoding]>32||[trim rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location!=NSNotFound){*error=@"名称不能为空，最多32字节，不含换行或制表符 / Invalid name (32 UTF-8 bytes max)";ok=NO;}else self.users[@(uid)]=trim;
  }else if([operation isEqual:@"select"]||[operation isEqual:@"reserve"])self.selected=uid;
  else {*error=@"Unknown user operation";ok=NO;}
  // Advance only after validation/archive succeeded; a failed validation must not
  // invalidate live processes' snapshots. Later publication failure is fail-closed.
  if(ok&&[operation isEqual:@"delete"]&&!mng_user_next_epoch(userLock)){
   *error=@"无法更新用户版本 / Cannot update user generation";ok=NO;
  }
  BOOL metadata=[@[@"avatar",@"delete",@"add"] containsObject:operation];
  if(ok&&metadata){
   ok=[self.avatars writeToFile:[self.root stringByAppendingPathComponent:@"avatars.plist"] atomically:YES];
   if(!ok)*error=@"无法保存头像 / Cannot save avatar";
  }
  if(ok&&![operation isEqual:@"avatar"]){
   NSMutableString *text=[NSMutableString stringWithFormat:@"selected\t%lu\n",(unsigned long)self.selected];
   for(NSNumber *id in self.ids)[text appendFormat:@"%@\t%@\n",id,self.users[id]];
   NSError *err=nil;ok=[text writeToFile:[self.root stringByAppendingPathComponent:@"users.tsv"] atomically:YES encoding:NSUTF8StringEncoding error:&err];
   if(!ok)*error=err.localizedDescription;
  }
 }
 if(!ok&&movedFrom){
  if(renamex_np(movedTo.fileSystemRepresentation,movedFrom.fileSystemRepresentation,RENAME_EXCL))
   *error=[NSString stringWithFormat:@"操作失败，存档保留在 %@ / Save preserved in archive",movedTo];
 }
 if(ok&&[operation isEqual:@"reserve"]){
  if(self.reservationFD>=0)close(self.reservationFD);
  self.reservationFD=userLock;userLock=-1;
 }
 if(userLock>=0)close(userLock);
 close(fd);return ok;
}
- (BOOL)reload:(NSString **)error { return [self transaction:nil user:0 name:nil error:error]; }
- (BOOL)edit:(NSString *)operation user:(NSUInteger)uid name:(NSString *)name error:(NSString **)error { return [self transaction:operation user:uid name:name error:error]; }
@end

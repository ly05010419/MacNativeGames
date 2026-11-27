#import <UIKit/UIKit.h>
#include "../../runtime/user_store.h"
#include "../../runtime/user_avatar.h"

static UIImage *mng_user_avatar_image(NSString *emoji) {
 UIGraphicsImageRenderer *renderer=[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(48,48)];
 return [renderer imageWithActions:^(UIGraphicsImageRendererContext *context){
  (void)context;[[UIColor.systemBlueColor colorWithAlphaComponent:0.1] setFill];
  [[UIBezierPath bezierPathWithOvalInRect:CGRectMake(0,0,48,48)] fill];
  NSDictionary *attributes=@{NSFontAttributeName:[UIFont systemFontOfSize:30]};CGSize size=[emoji sizeWithAttributes:attributes];
  [emoji drawAtPoint:CGPointMake((48-size.width)/2,(48-size.height)/2) withAttributes:attributes];
 }];
}
@interface MNGEmojiPicker : UICollectionViewController
@property(nonatomic,copy) void (^choose)(NSString *);
@end
@implementation MNGEmojiPicker
- (void)viewDidLoad {
 [super viewDidLoad];self.title=@"选择头像 / Avatar";self.collectionView.backgroundColor=UIColor.systemBackgroundColor;
 [self.collectionView registerClass:UICollectionViewCell.class forCellWithReuseIdentifier:@"emoji"];
}
- (NSInteger)collectionView:(UICollectionView *)view numberOfItemsInSection:(NSInteger)section {(void)view;(void)section;return mng_user_avatar_choices().count;}
- (UICollectionViewCell *)collectionView:(UICollectionView *)view cellForItemAtIndexPath:(NSIndexPath *)path {
 UICollectionViewCell *cell=[view dequeueReusableCellWithReuseIdentifier:@"emoji" forIndexPath:path];
 for(UIView *old in cell.contentView.subviews)[old removeFromSuperview];
 UILabel *label=[[UILabel alloc] initWithFrame:cell.contentView.bounds];label.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
 label.font=[UIFont systemFontOfSize:36];label.textAlignment=NSTextAlignmentCenter;label.text=mng_user_avatar_choices()[path.item];[cell.contentView addSubview:label];
 cell.isAccessibilityElement=YES;cell.accessibilityLabel=label.text;cell.accessibilityTraits=UIAccessibilityTraitButton;return cell;
}
- (void)collectionView:(UICollectionView *)view didSelectItemAtIndexPath:(NSIndexPath *)path {
 (void)view;if(self.choose)self.choose(mng_user_avatar_choices()[path.item]);[self.navigationController popViewControllerAnimated:YES];
}
@end
@interface MNGUserPicker : UITableViewController
@property(nonatomic,strong) MNGUserStore *store;
@property(nonatomic,copy) void (^onStart)(void);
@end
@implementation MNGUserPicker
- (void)viewDidLoad {
 [super viewDidLoad];self.tableView.rowHeight=76;self.title=@"存档用户 / Save users";
 self.navigationItem.leftBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:@"新增 / Add" style:UIBarButtonItemStylePlain target:self action:@selector(addUser)];
 self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:@"开始 / Play" style:UIBarButtonItemStyleDone target:self action:@selector(play)];
 UILabel *note=[[UILabel alloc] initWithFrame:CGRectMake(0,0,360,64)];note.numberOfLines=0;note.textAlignment=NSTextAlignmentCenter;
 note.text=@"独立存档 · 点击选用户，ⓘ 管理，头像可更换\nSeparate saves · Select a user; ⓘ manages profile";self.tableView.tableHeaderView=note;
}
- (void)viewDidAppear:(BOOL)animated {
 [super viewDidAppear:animated];[self refresh];
}
- (void)showError:(NSString *)error {
 UIAlertController *a=[UIAlertController alertControllerWithTitle:@"用户操作失败 / User error" message:error preferredStyle:UIAlertControllerStyleAlert];
 [a addAction:[UIAlertAction actionWithTitle:@"好 / OK" style:UIAlertActionStyleDefault handler:nil]];[self presentViewController:a animated:YES completion:nil];
}
- (void)refresh {NSString *error=nil;if(![self.store reload:&error]){self.navigationItem.rightBarButtonItem.enabled=NO;[self showError:error];return;}self.navigationItem.rightBarButtonItem.enabled=YES;[self.tableView reloadData];}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {(void)tableView;(void)section;return self.store.ids.count;}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)index {
 (void)tableView;UITableViewCell *cell=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];NSNumber *uid=self.store.ids[index.row];
 cell.textLabel.text=[self.store title:uid];cell.detailTextLabel.text=uid.unsignedIntegerValue==self.store.selected?@"✓ 当前用户 / Selected":@"独立游戏进度 / Separate progress";
 cell.imageView.image=mng_user_avatar_image([self.store avatarForUser:uid.unsignedIntegerValue]);
 cell.imageView.isAccessibilityElement=YES;cell.imageView.accessibilityLabel=@"更换头像 / Change avatar";
 cell.imageView.userInteractionEnabled=YES;cell.imageView.tag=uid.integerValue;
 [cell.imageView addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(avatarTapped:)]];
 cell.textLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
 cell.textLabel.adjustsFontForContentSizeCategory=YES;
 cell.detailTextLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
 cell.detailTextLabel.adjustsFontForContentSizeCategory=YES;
 if(uid.unsignedIntegerValue==self.store.selected)cell.accessibilityTraits|=UIAccessibilityTraitSelected;
 cell.accessoryType=UITableViewCellAccessoryDetailButton;return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)index {
 (void)tableView;NSString *error=nil;NSUInteger uid=self.store.ids[index.row].unsignedIntegerValue;
 if(![self.store edit:@"select" user:uid name:nil error:&error])[self showError:error];[self refresh];
}
- (void)renameUser:(NSNumber *)uid {
 UIAlertController *a=[UIAlertController alertControllerWithTitle:@"用户备注 / Rename" message:@"不改变ID或存档；最多32个UTF-8字节。\nDoes not change ID or saves; 32 UTF-8 bytes max." preferredStyle:UIAlertControllerStyleAlert];
 [a addTextFieldWithConfigurationHandler:^(UITextField *f){f.text=self.store.users[uid];}];
 [a addAction:[UIAlertAction actionWithTitle:@"取消 / Cancel" style:UIAlertActionStyleCancel handler:nil]];
 [a addAction:[UIAlertAction actionWithTitle:@"保存 / Save" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){(void)action;NSString *error=nil;if(![self.store edit:@"rename" user:uid.unsignedIntegerValue name:a.textFields.firstObject.text error:&error])[self showError:error];[self refresh];}]];
 [self presentViewController:a animated:YES completion:nil];
}
- (void)avatarTapped:(UITapGestureRecognizer *)gesture { [self changeAvatar:@(gesture.view.tag)]; }
- (void)changeAvatar:(NSNumber *)uid {
 UICollectionViewFlowLayout *layout=[UICollectionViewFlowLayout new];layout.itemSize=CGSizeMake(64,64);layout.sectionInset=UIEdgeInsetsMake(20,20,20,20);
 MNGEmojiPicker *picker=[[MNGEmojiPicker alloc] initWithCollectionViewLayout:layout];
 __weak MNGUserPicker *weakSelf=self;
 picker.choose=^(NSString *emoji){MNGUserPicker *owner=weakSelf;NSString *error=nil;if(![owner.store edit:@"avatar" user:uid.unsignedIntegerValue name:emoji error:&error])[owner showError:error];[owner refresh];};
 [self.navigationController pushViewController:picker animated:YES];
}
- (void)deleteUser:(NSNumber *)uid {
 UIAlertController *alert=[UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"删除 %@？",[self.store title:uid]] message:@"存档将移至 profile/deleted-users 保留，ID可以复用，新用户不会继承旧进度。至少保留一位用户。\nSaves are archived, not erased." preferredStyle:UIAlertControllerStyleAlert];
 [alert addAction:[UIAlertAction actionWithTitle:@"取消 / Cancel" style:UIAlertActionStyleCancel handler:nil]];
 [alert addAction:[UIAlertAction actionWithTitle:@"删除 / Delete" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action){(void)action;NSString *error=nil;if(![self.store edit:@"delete" user:uid.unsignedIntegerValue name:nil error:&error])[self showError:error];[self refresh];}]];
 [self presentViewController:alert animated:YES completion:nil];
}
- (void)tableView:(UITableView *)tableView accessoryButtonTappedForRowWithIndexPath:(NSIndexPath *)index {
 NSNumber *uid=self.store.ids[index.row];
 UIAlertController *menu=[UIAlertController alertControllerWithTitle:[self.store title:uid] message:nil preferredStyle:UIAlertControllerStyleActionSheet];
 [menu addAction:[UIAlertAction actionWithTitle:@"更换头像 / Avatar" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){(void)action;[self changeAvatar:uid];}]];
 [menu addAction:[UIAlertAction actionWithTitle:@"改名 / Rename" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){(void)action;[self renameUser:uid];}]];
 [menu addAction:[UIAlertAction actionWithTitle:@"删除用户 / Delete" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *action){(void)action;[self deleteUser:uid];}]];
 [menu addAction:[UIAlertAction actionWithTitle:@"取消 / Cancel" style:UIAlertActionStyleCancel handler:nil]];
 menu.popoverPresentationController.sourceView=tableView;menu.popoverPresentationController.sourceRect=[tableView rectForRowAtIndexPath:index];
 [self presentViewController:menu animated:YES completion:nil];
}
- (void)addUser {NSString *error=nil;if(![self.store edit:@"add" user:0 name:nil error:&error])[self showError:error];[self refresh];}
- (void)play {
 NSString *error=nil;if(![self.store edit:@"select" user:self.store.selected name:nil error:&error]){[self showError:error];return;}
 setenv("MNG_USER_ID",[[NSString stringWithFormat:@"%lu",(unsigned long)self.store.selected] UTF8String],1);
 self.navigationItem.rightBarButtonItem.enabled=NO;
 void (^start)(void)=self.onStart;self.onStart=nil;[self dismissViewControllerAnimated:YES completion:start];
}
@end
static void mng_present_user_picker(UIViewController *host,NSString *profile,void (^start)(void)) {
 MNGUserPicker *picker=[[MNGUserPicker alloc] initWithStyle:UITableViewStyleInsetGrouped];
 picker.store=[[MNGUserStore alloc] initWithRoot:profile];picker.onStart=start;
 UINavigationController *nav=[[UINavigationController alloc] initWithRootViewController:picker];nav.modalPresentationStyle=UIModalPresentationFullScreen;
 [host presentViewController:nav animated:YES completion:nil];
}

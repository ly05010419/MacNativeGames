/* Main queue owns the dialog. Only the calling guest thread waits; UIKit and
   the other guest threads remain live. No automatic name or save edits. */
#include "text_input_filter.h"
static __weak UIView *textInputHost;
@interface D3TextInputSession : NSObject <UITextFieldDelegate>
@property(strong) UIAlertController *alert;
@property(strong) UIAlertAction *confirm;
@property(strong) id backgroundObserver;
@property(copy) void (^completion)(NSString *);
@property int maxUnits, minUnits, digitsOnly, previousSuspended;
@property uint32_t disabled;
@property BOOL finished;
- (void)changed:(UITextField *)field;
- (void)finish:(NSString *)text;
@end
static D3TextInputSession *textInputSession;
@implementation D3TextInputSession
- (void)changed:(UITextField *)field {
    if (field.markedTextRange) { self.confirm.enabled=NO; return; }
    NSString *clean=runtime_text_filtered(field.text?:@"",self.maxUnits,self.digitsOnly,self.disabled);
    if (![clean isEqualToString:field.text]) field.text=clean;
    self.confirm.enabled=clean.length>=(NSUInteger)MAX(0,self.minUnits);
}
- (BOOL)textFieldShouldReturn:(UITextField *)field {
    [self changed:field];
    if (self.confirm.enabled) [self finish:field.text?:@""];
    return NO;
}
- (void)finish:(NSString *)text {
    if (self.finished) return;
    self.finished=YES;
    if (self.backgroundObserver) [NSNotificationCenter.defaultCenter removeObserver:self.backgroundObserver];
    self.backgroundObserver=nil;
    void (^done)(NSString *)=self.completion; self.completion=nil;
    void (^complete)(void)=^{
        runtime_input_settings_suspend(self.previousSuspended);
        if (textInputSession==self) textInputSession=nil;
        if (done) done(text);
    };
    if (self.alert.presentingViewController) [self.alert dismissViewControllerAnimated:NO completion:complete];
    else complete();
}
@end
int runtime_front_text_input(const RuntimeTextRequest *req,char *out,size_t out_size) {
    if (!req||!out||!out_size) return 0;
    out[0]=0;
    if (NSThread.isMainThread) {
        fprintf(stderr,"[swkbd-ios] refusing to block the UIKit thread\n"); return 0;
    }
    NSString *prompt=[NSString stringWithUTF8String:req->prompt?:""]?:@"";
    NSString *initial=[NSString stringWithUTF8String:req->initial?:""]?:@"";
    int maxUnits=req->max_units,minUnits=req->min_units,digits=req->digits_only,secure=req->secure;
    uint32_t disabled=req->disabled;
    dispatch_semaphore_t semaphore=dispatch_semaphore_create(0);
    __block NSString *result=nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *presenter=textInputHost.window.rootViewController;
        while (presenter.presentedViewController) presenter=presenter.presentedViewController;
        if (!presenter||!presenter.view.window||presenter.isBeingDismissed||textInputSession||
            UIApplication.sharedApplication.applicationState!=UIApplicationStateActive) {
            fprintf(stderr,"[swkbd-ios] no available active presenter; cancelled\n");
            dispatch_semaphore_signal(semaphore); return;
        }
        D3TextInputSession *session=[D3TextInputSession new];textInputSession=session;
        session.maxUnits=maxUnits;session.minUnits=minUnits;session.digitsOnly=digits;session.disabled=disabled;
        session.previousSuspended=runtime_input_settings_active();
        session.completion=^(NSString *text){result=[text copy];dispatch_semaphore_signal(semaphore);};
        UIAlertController *alert=[UIAlertController alertControllerWithTitle:prompt message:nil preferredStyle:UIAlertControllerStyleAlert];
        session.alert=alert;
        __weak D3TextInputSession *weak=session;
        [alert addTextFieldWithConfigurationHandler:^(UITextField *field){
            field.text=initial;field.secureTextEntry=secure;field.keyboardType=digits?UIKeyboardTypeNumberPad:UIKeyboardTypeDefault;
            field.autocorrectionType=UITextAutocorrectionTypeNo;field.spellCheckingType=UITextSpellCheckingTypeNo;
            field.autocapitalizationType=UITextAutocapitalizationTypeNone;field.returnKeyType=UIReturnKeyDone;
            field.delegate=weak;[field addTarget:weak action:@selector(changed:) forControlEvents:UIControlEventEditingChanged];
        }];
        session.confirm=[UIAlertAction actionWithTitle:@(TOTK_UI("确定","OK")) style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){
            (void)action;D3TextInputSession *s=weak;if(!s)return;
            [s changed:s.alert.textFields.firstObject];
            [s finish:s.confirm.enabled?s.alert.textFields.firstObject.text:nil];
        }];
        [alert addAction:[UIAlertAction actionWithTitle:@(TOTK_UI("取消","Cancel")) style:UIAlertActionStyleCancel handler:^(UIAlertAction *action){(void)action;[weak finish:nil];}]];
        [alert addAction:session.confirm];alert.preferredAction=session.confirm;
        [session changed:alert.textFields.firstObject];
        session.backgroundObserver=[NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note){(void)note;[weak finish:nil];}];
        runtime_input_settings_suspend(1);
        [presenter presentViewController:alert animated:NO completion:^{
            if (!alert.presentingViewController) [weak finish:nil];
        }];
        fprintf(stderr,"[swkbd-ios] text dialog shown (limit %d UTF-16 units)\n",maxUnits);
    });
    dispatch_semaphore_wait(semaphore,DISPATCH_TIME_FOREVER);
    if (!result) return 0;
    if (![result getCString:out maxLength:out_size encoding:NSUTF8StringEncoding]) {out[0]=0;return 0;}
    return 1;
}

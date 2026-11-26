#ifndef MNG_USER_AVATAR_H
#define MNG_USER_AVATAR_H
/* Local, user-selected system emoji. No downloaded images or ID-derived avatar. */
#import <Foundation/Foundation.h>
static NSArray<NSString *> *mng_user_avatar_choices(void) {
    return @[@"😀", @"😎", @"🥳", @"🤖", @"👻", @"👽", @"🐱", @"🐶", @"🦊", @"🐼", @"🐸", @"🐧", @"🐯", @"🦁", @"🐰", @"🐨", @"🐻", @"🦄"];
}

#endif

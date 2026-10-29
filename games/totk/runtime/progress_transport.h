#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN
/* Network/file-provider work never runs on the store or game-save queue. */
@interface TOTKProgressTransport : NSObject
- (instancetype)initWithLocalRoot:(NSURL *)root;
- (void)exchangeWithFolder:(NSURL *)folder completion:(void (^)(NSArray<NSURL *> *incoming, NSArray<NSString *> *problems))completion;
@end
NS_ASSUME_NONNULL_END

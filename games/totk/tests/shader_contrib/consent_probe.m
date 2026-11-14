#import <Foundation/Foundation.h>
#include "totk_contrib_upload.h"

extern NSString *other_choice(NSString *);
extern NSUInteger other_send(NSString *);
extern void *other_gate_address(void);
extern void *other_timer_address(void);

static int failures, checked;
static void check(BOOL ok, const char *message) {
    checked++;
    if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", message); }
}
static NSString *get(NSString *path) {
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] ?: @"";
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 3) return 2;
        NSString *mode = @(argv[1]), *path = @(argv[2]);
        check(other_gate_address() == &totk_inbox_session_off, "session gate shared across translation units");
        check(other_timer_address() == &totk_inbox_timer, "timer slot shared across translation units");
        NSString *choice = nil;
        NSUInteger sent = 0;
        if ([mode isEqualToString:@"inspect"]) {
            choice = other_choice(path);
            check(choice == nil, "quarantined game does not inherit a choice");
            check(totk_inbox_should_ask(path) != 0, "new game must be asked explicitly");
            check(other_send(path) == 0, "no consent means no direct upload either");
        } else if ([mode isEqualToString:@"claim-failure"]) {
            NSString *before = get(path), *upload = totk_contrib_side(path, @"upload");
            NSString *previousChoice = get(upload);
            NSString *parent = path.stringByDeletingLastPathComponent;
            chmod(parent.fileSystemRepresentation, 0500);
            NSString *claimed = totk_inbox_claim(path);
            chmod(parent.fileSystemRepresentation, 0700);
            check([claimed isEqualToString:path], "failed claim preserves the path API for existing launchers");
            check([get(path) isEqualToString:before], "failed quarantine preserves foreign main file");
            check([get(upload) isEqualToString:previousChoice], "failed quarantine preserves old consent bytes");
            NSString *seed = [parent stringByAppendingPathComponent:@"seed"], *lists = [parent stringByAppendingPathComponent:@"lists"];
            for (NSString *directory in @[seed, lists])
                [NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil];
            [@"{}\n" writeToFile:[seed stringByAppendingPathComponent:@"seed-manifest.json"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
            [@"R\tabcd\n" writeToFile:[lists stringByAppendingPathComponent:@"pso_manifest.tsv"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
            check(totk_contrib_prune(path, seed, lists) == 0 && [get(path) isEqualToString:before], "failed claim cannot prune matching foreign rows");
            choice = other_choice(path);
            check([choice isEqualToString:@"off"], "failed quarantine is off for this run across TUs");
            check(other_send(path) == 0, "failed quarantine starts no request");
        } else {
            NSMutableString *text = [NSMutableString stringWithString:@"# Inbox game: oot\n"];
            unsigned count = [mode isEqualToString:@"chunk-stop"] ? 4001 : 1;
            for (unsigned i = 0; i < count; i++) [text appendFormat:@"V\t%064x\t{}\n", i + 1];
            [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
            if ([mode isEqualToString:@"chunk-stop"]) {
                check(totk_inbox_choose(path, YES), "explicit opt-in persisted");
                sent = other_send(path);   // loopback fixture turns .upload off during first chunk
                check(sent == TOTK_INBOX_CHUNK, "only first in-flight chunk accepted before opt-out");
                check(totk_contrib_read(path).count == count - TOTK_INBOX_CHUNK, "later chunks stay unsent");
                check(totk_inbox_sent(path) == TOTK_INBOX_CHUNK, "only accepted chunk counted");
                check(totk_inbox_send(path, @"mac") == 0, "later send stays off");
                choice = other_choice(path);
            } else if ([mode isEqualToString:@"failed-off"]) {
                check(totk_inbox_choose(path, YES), "old auto preference persisted");
                NSString *parent = path.stringByDeletingLastPathComponent;
                chmod(parent.fileSystemRepresentation, 0500);
                BOOL saved = totk_inbox_choose(path, NO);
                chmod(parent.fileSystemRepresentation, 0700);
                check(!saved, "read-only parent makes preference replacement fail");
                check([get(totk_contrib_side(path, @"upload")) hasPrefix:@"auto"], "disk choice remains auto in failure fixture");
                choice = other_choice(path);
                check([choice isEqualToString:@"off"], "effective session choice is off in other TU");
                check(other_send(path) == 0, "failed persistence still prevents requests");
                check(totk_inbox_choose(path, YES), "later explicit successful opt-in can restore consent");
                check([other_choice(path) isEqualToString:@"auto"], "successful opt-in clears shared session gate");
            } else if ([mode isEqualToString:@"failed-on"]) {
                check(totk_inbox_choose(path, NO), "off preference persisted");
                NSString *parent = path.stringByDeletingLastPathComponent;
                chmod(parent.fileSystemRepresentation, 0500);
                BOOL saved = totk_inbox_choose(path, YES);
                chmod(parent.fileSystemRepresentation, 0700);
                check(!saved, "failed opt-in reported");
                choice = other_choice(path);
                check([choice isEqualToString:@"off"], "failed opt-in leaves effective choice off");
                check(other_send(path) == 0, "failed opt-in starts no request");
            } else return 2;
        }
        NSDictionary *result = @{@"status": failures ? @"fail" : @"pass", @"checked": @(checked),
                                 @"failures": @(failures), @"mode": mode, @"sent": @(sent),
                                 @"choice": choice ?: NSNull.null};
        NSData *json = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
        fwrite(json.bytes, 1, json.length, stdout); fputc('\n', stdout);
        return failures ? 1 : 0;
    }
}

/* The Update page's check and install (games/totk/app/auto_update.h) on real git repositories in a temporary folder: an upstream,
   the player's checkout of it, and a stand-in for update_runtime.py that records how it was run. Nothing outside the
   folder is read or written. */
#include "../games/totk/app/auto_update.h"
#include <assert.h>

static NSString *root, *upstream, *other, *checkout, *folder, *record, *python;

static NSString *sh(NSString *dir, NSString *command) {
    NSString *out = nil;
    int st = mng_update_command(dir, @[@"/bin/sh", @"-c", command], 60, nil, &out);
    if (st) { fprintf(stderr, "failed (%d): %s\n%s\n", st, command.UTF8String, out.UTF8String); abort(); }
    return [out stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}
static NSString *head(NSString *repo) { return sh(repo, @"git rev-parse --short=7 HEAD"); }
/* A new commit upstream (made in another clone and pushed). */
static NSString *publish(NSString *title) {
    sh(other, [NSString stringWithFormat:@"git pull -q && echo '%@' >> notes.txt && git commit -qam '%@' && git push -q", title, title]);
    return head(other);
}
static NSDictionary *status(void) {
    return [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[folder stringByAppendingPathComponent:@"status.json"]] options:0 error:nil];
}
static NSArray<NSString *> *runs(void) {
    NSString *text = [NSString stringWithContentsOfFile:record encoding:NSUTF8StringEncoding error:nil] ?: @"";
    NSMutableArray *lines = [[text componentsSeparatedByString:@"\n"] mutableCopy]; [lines removeObject:@""]; return lines;
}
static NSMutableArray<NSString *> *told;   /* what the launcher's window was told, one "to total" per update_runtime.py run */
static NSString *install(NSString *app_commit) {
    return mng_update_install(folder, checkout, [root stringByAppendingPathComponent:@"Game.app"], app_commit, python, [root stringByAppendingPathComponent:@"TOTK_build"],
                              ^(NSString *to, NSInteger total) { [told addObject:[NSString stringWithFormat:@"%@ %ld", to, (long)total]]; });
}
static NSString *check(NSString *app_commit) { return mng_update_check(folder, checkout, app_commit)[@"state"]; }
static void expect(NSString *got, NSString *state, NSString *reason) {
    NSDictionary *d = status();
    if (![got isEqualToString:state] || ![d[@"state"] isEqualToString:state] || (reason && ![d[@"reason"] isEqualToString:reason])) {
        fprintf(stderr, "got %s, status %s / %s; expected %s / %s\n", got.UTF8String, [d[@"state"] UTF8String], [d[@"reason"] UTF8String], state.UTF8String, reason.UTF8String);
        abort();
    }
}

int main(void) { @autoreleasepool {
    char base[] = "/tmp/auto-update-probe-XXXXXX"; assert(mkdtemp(base));
    root = @(base); upstream = [root stringByAppendingPathComponent:@"upstream.git"]; other = [root stringByAppendingPathComponent:@"other"];
    checkout = [root stringByAppendingPathComponent:@"checkout"]; folder = [root stringByAppendingPathComponent:@"data/update"];
    record = [root stringByAppendingPathComponent:@"runs.txt"]; python = [root stringByAppendingPathComponent:@"python"];
    setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1); setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
    setenv("GIT_AUTHOR_NAME", "t", 1); setenv("GIT_AUTHOR_EMAIL", "t@t", 1); setenv("GIT_COMMITTER_NAME", "t", 1); setenv("GIT_COMMITTER_EMAIL", "t@t", 1);
    [NSFileManager.defaultManager createDirectoryAtPath:[root stringByAppendingPathComponent:@"TOTK_build"] withIntermediateDirectories:YES attributes:nil error:nil];
    /* The stand-in: records its arguments; with fail-seed present it stops as update_runtime.py does for a new chain unless
       given --seed; with fail present it always stops. */
    NSString *fake = [NSString stringWithFormat:@"#!/bin/sh\necho \"$*\" >> '%@'\n"
        "if [ -e '%@/fail' ]; then echo 'building'; echo 'ERROR: the build stopped'; exit 1; fi\n"
        "case \"$*\" in *--seed*) ;; *) if [ -e '%@/fail-seed' ]; then echo 'ERROR: the app needs a new seed. Pass --seed to make it'; exit 1; fi;; esac\n"
        "exit 0\n", record, root, root];
    assert([fake writeToFile:python atomically:YES encoding:NSUTF8StringEncoding error:nil]); sh(root, @"chmod +x python");
    sh(root, @"git init -q --bare -b main upstream.git && git clone -q upstream.git other 2>/dev/null");
    sh(other, @"git checkout -q -b main && touch update_runtime.py && echo a > notes.txt && git add . && git commit -qm first && git push -q -u origin main");
    sh(root, @"git clone -q upstream.git checkout");
    NSString *built = head(checkout); told = [NSMutableArray array];

    /* The checkout is the app's version: nothing to do. */
    expect(check(built), @"latest", nil); expect(install(built), @"latest", nil); assert(runs().count == 0 && told.count == 0);
    /* Upstream moved on: the check lists it and leaves the checkout alone; the install fast-forwards it and runs
       update_runtime.py for this app with its work folder. */
    publish(@"fix: one"); NSString *two = publish(@"feat: two");
    expect(check(built), @"behind", nil); assert([head(checkout) isEqualToString:built] && runs().count == 0);
    NSDictionary *d = status();
    assert([d[@"from"] isEqualToString:built] && [d[@"to"] isEqualToString:two] && [d[@"total"] isEqual:@2] && [d[@"installed"] isEqualToString:built]);
    assert([d[@"commits"] isEqual:(@[@"feat: two", @"fix: one"])] && [d[@"version"] isEqual:@1] && [d[@"at"] doubleValue] > 1.7e9);
    expect(install(built), @"updated", nil);
    assert([head(checkout) isEqualToString:two] && runs().count == 1);
    NSString *asked = [NSString stringWithFormat:@"update_runtime.py %@/Game.app --work %@/TOTK_build", root, root];
    assert([runs()[0] isEqualToString:asked] && told.count == 1 && [told[0] isEqualToString:[two stringByAppendingString:@" 2"]]);   /* the window says what comes */
    assert([status()[@"installed"] isEqualToString:two] && [status()[@"commits"] count] == 2);
    /* The next start runs the new version: up to date. */
    expect(check(two), @"latest", nil);
    /* An app built from a commit the checkout does not know: behind, the commits not told. */
    expect(check(@"0000000"), @"behind", nil); assert([status()[@"total"] isEqual:@0] && ![status()[@"commits"] count]);

    /* A working copy is never moved: uncommitted changes, another branch, commits upstream lacks. The check says why and
       still lists what upstream has; the install does nothing. */
    publish(@"fix: three"); NSString *before = head(checkout);
    sh(checkout, @"echo x >> notes.txt"); expect(check(two), @"blocked", @"local_changes"); expect(install(two), @"blocked", @"local_changes");
    assert([status()[@"commits"] isEqual:@[@"fix: three"]] && [status()[@"total"] isEqual:@1] && [status()[@"detail"] isEqualToString:@"M notes.txt"]);
    sh(checkout, @"git checkout -q notes.txt");
    sh(checkout, @"git checkout -q -b mine"); expect(check(two), @"blocked", @"not_main"); sh(checkout, @"git checkout -q main && git branch -q -D mine");
    sh(checkout, @"echo y > mine.txt && git add mine.txt && git commit -qm mine"); expect(install(two), @"blocked", @"unpushed");
    sh(checkout, @"git reset -q --hard HEAD~1");
    assert([head(checkout) isEqualToString:before] && runs().count == 1);
    /* Untracked files are not changes to the checkout. */
    sh(checkout, @"echo z > untracked.txt");
    expect(install(two), @"updated", nil); assert(runs().count == 2);
    NSString *three = head(checkout);

    /* A new translation chain: update_runtime.py asks for --seed and gets it. */
    sh(root, @"touch fail-seed"); NSString *four = publish(@"feat: four");
    expect(install(three), @"updated", nil);
    assert(runs().count == 4 && ![runs()[2] containsString:@"--seed"] && [runs()[3] hasSuffix:@"--seed"] && [status()[@"to"] isEqualToString:four]);
    sh(root, @"rm fail-seed");
    /* A failure: the app stays as it was and the reason is kept (the page and the launcher tell the player to update by hand). */
    sh(root, @"touch fail"); NSString *five = publish(@"fix: five");
    expect(install(four), @"failed", @"update_failed");
    assert([status()[@"detail"] isEqualToString:@"ERROR: the build stopped"] && [status()[@"installed"] isEqualToString:four] && [status()[@"to"] isEqualToString:five]);
    sh(root, @"rm fail");

    /* No checkout recorded (an app made before), one that is gone, or one that cannot reach upstream. */
    expect([mng_update_check(folder, nil, five)[@"state"] description], @"error", @"no_repo");
    expect([mng_update_check(folder, root, five)[@"state"] description], @"error", @"no_repo");
    sh(checkout, @"git remote set-url origin /nonexistent/upstream.git"); expect(check(five), @"error", @"fetch_failed");
    sh(checkout, [NSString stringWithFormat:@"git remote set-url origin '%@'", upstream]);

    /* An agent's test session never updates; the lock lets one run at a time. */
    unsetenv("TOTK_SLOT_OWNER"); assert(!mng_update_held());
    setenv("TOTK_SLOT_OWNER", "user", 1); assert(!mng_update_held());
    setenv("TOTK_SLOT_OWNER", "claude", 1); assert(mng_update_held()); unsetenv("TOTK_SLOT_OWNER");
    int lock = mng_update_lock(folder); assert(lock >= 0 && mng_update_lock(folder) < 0); close(lock);
    NSString *data = [root stringByAppendingPathComponent:@"data"];
    setenv("TOTK_CONTROLS", [data stringByAppendingPathComponent:@"controls.txt"].UTF8String, 1); assert([mng_update_data() isEqualToString:data]);

    sh(@"/", [NSString stringWithFormat:@"rm -rf '%@'", root]);
    puts("pass: auto update");
    return 0;
} }

// totk_contrib_upload.h: the contribution file sent to the shader inbox. Built with -DTOTK_CONTRIB_INBOX="<url>".
// Usage: inbox_probe <file> [up | down]; prints JSON. up: the inbox at the URL takes the rows, which leave the file for
// its done file, once; down: nothing answers at the URL, the rows stay for the next try. tests/test_shader_contrib.py
// ShaderInboxUploadTests runs it against the inbox's test/local-server.js (the MacNativeGamesWeb project, shader-inbox/).
#import <Foundation/Foundation.h>
#define TOTK_GAME_VERSION "1.4.3"
#include "totk_shader_contrib.h"
#include "totk_contrib_upload.h"

static int failures, checked;
static void check(int ok, const char *what) { checked++; if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", what); } }
static NSString *get(NSString *path) { return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] ?: @""; }

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 3) { fprintf(stderr, "usage: inbox_probe <file> up|down\n"); return 2; }
        NSString *path = @(argv[1]);
        BOOL up = !strcmp(argv[2], "up");
        NSString *v = [NSString stringWithFormat:@"V\t%064x\t{\"attr\":{},\"fout\":{},\"img\":{},\"tex\":{}}", 7];
        NSString *r = @"R\tabcdef", *b = @"B\tabcdef\t-\t0:1:0x0p+0,0x0p+0,0x0p+0,0x1p+0;";
        [[NSString stringWithFormat:@"# header\n# Game: TOTK 1.4.3\n%@\n%@\n%@\n", v, r, b]
            writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];

        check(totk_inbox_configured(), "the URL is built in");
        check(totk_inbox_choice(path) == nil, "not asked yet");
        totk_inbox_choose(path, NO);
        check([totk_inbox_choice(path) isEqualToString:@"off"], "off is kept");
        totk_inbox_choose(path, YES);
        check([totk_inbox_choice(path) isEqualToString:@"auto"], "auto is kept");

        NSUInteger sent = totk_inbox_send(path, @"mac");
        NSString *install = get(totk_contrib_side(path, @"install"));
        check(install.length == 36 && [install isEqualToString:install.lowercaseString], "a lowercase UUID install id");
        if (up) {
            check(sent == 3, "the three rows are taken");
            check(totk_contrib_read(path).count == 0, "they leave the file");
            check([get(path) containsString:@"# Game: TOTK 1.4.3"], "header and game lines stay");
            check(totk_contrib_read(totk_contrib_side(path, @"done")).count == 3, "they go to the done file");
            check(totk_inbox_send(path, @"mac") == 0, "nothing is sent twice");
            check([get(totk_contrib_side(path, @"install")) isEqualToString:install], "the install id stays");
        } else {
            check(sent == 0, "nothing is taken");
            check(totk_contrib_read(path).count == 3, "the rows stay for the next try");
            check(totk_contrib_read(totk_contrib_side(path, @"done")).count == 0, "nothing is done");
        }
        printf("{\"status\":\"%s\",\"checked\":%d,\"failures\":%d,\"install\":\"%s\"}\n",
               failures ? "fail" : "pass", checked, failures, install.UTF8String);
        return failures ? 1 : 0;
    }
}

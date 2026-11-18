// Whose contribution file it is (tests/test_shader_contrib.py ContributionOwnerTests): built against each game's own copy of
// totk_contrib_upload.h and totk_shader_contrib.h. Every game writes and sends only a file of its own id (totk_inbox_claim).
//   owner_probe id                  prints the copy's GAME_INBOX_ID
//   owner_probe claim <file>        claims the file for this game (a file of another game or of none moves aside)
//   owner_probe write <file>        writes one row as the game does (TOTK_SHADER_CONTRIB = <file>; the id from the environment)
//   owner_probe claim-write <file>  both, as a launcher and its game do
//   owner_probe send <file>         one upload round (TOTK_CONTRIB_INBOX given at build time)
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <string.h>
#include "totk_contrib_upload.h"

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "id")) { printf("%s\n", GAME_INBOX_ID); return 0; }
        if (argc != 3) { fprintf(stderr, "usage: owner_probe id|claim|write|claim-write|send <file>\n"); return 2; }
        NSString *path = @(argv[2]);
        BOOL claim = !strcmp(argv[1], "claim") || !strcmp(argv[1], "claim-write");
        BOOL write = !strcmp(argv[1], "write") || !strcmp(argv[1], "claim-write");
        if (claim) totk_inbox_claim(path);
        if (write) {
            setenv("TOTK_SHADER_CONTRIB", path.fileSystemRepresentation, 1);
            totk_contrib_add([NSString stringWithFormat:@"%064x", 7], @{});   // every copy's writer (a V row)
        }
        if (!strcmp(argv[1], "send")) printf("%lu\n", (unsigned long)totk_inbox_send(path, @"mac"));
        else if (!claim && !write) { fprintf(stderr, "unknown mode %s\n", argv[1]); return 2; }
    }
    return 0;
}

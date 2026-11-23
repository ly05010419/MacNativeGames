/* Where the game's SD card paths land on the Mac (runtime/sdmc_path.h). */
#include "../runtime/sdmc_path.h"
#include <stdio.h>
static int fails;
static void ok(const char *guest, const char *want) {
    char out[512]; int r = sdmc_host_path("/P", guest, out, sizeof out);
    if (r || strcmp(out, want)) { fails++; printf("FAIL: %s -> %s (%d), want %s\n", guest, r ? "-" : out, r, want); }
}
static void refused(const char *guest) {
    char out[512]; if (!sdmc_host_path("/P", guest, out, sizeof out)) { fails++; printf("FAIL: %s was accepted as %s\n", guest, out); }
}
int main(void) {
    ok("/switch/mj2013/save/update.rom", "/P/sdmc/switch/mj2013/save/update.rom");
    ok("/switch/mj2013/save/system/font_l24.rom.bak", "/P/sdmc/switch/mj2013/save/system/font_l24.rom.bak");
    ok("/switch/mj2013/save/", "/P/sdmc/switch/mj2013/save");
    ok("/", "/P/sdmc/");
    ok("/a..b/c.d", "/P/sdmc/a..b/c.d");
    refused("switch/mj2013");            /* relative */
    refused("/switch/../../etc/passwd");
    refused("/..");
    refused("/switch/./x");
    refused("/switch//x");
    refused("/switch\\x");
    refused("/sw\x01itch");
    refused("");
    { char out[8]; if (!sdmc_host_path("/P", "/switch/mj2013", out, sizeof out)) { fails++; printf("FAIL: truncated path accepted\n"); } }
    { char longp[0x400]; memset(longp, 'a', sizeof longp - 1); longp[0] = '/'; longp[sizeof longp - 1] = 0; refused(longp); }
    printf(fails ? "sdmc FAILED\n" : "sdmc ok\n"); return fails != 0;
}

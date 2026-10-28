/* One WebDAV save sync from the command line (app/webdav_sync.h): the launcher's and the iPhone app's code path, without
   the game. The profile is laid out as the apps make it (profile/save/<account>, account.uid beside it), the server and
   its credentials come from TOTK_WEBDAV_URL / _USER / _PASSWORD; scripts/mini_webdav.py can stand in for the server:

     python3 games/totk/scripts/mini_webdav.py --root /tmp/dav --port 8442 &
     make -C games/totk probe-save-sync PROFILE=/tmp/save-sync-profile TOTK_WEBDAV_URL=http://127.0.0.1:8442/
 */
#import <Foundation/Foundation.h>
#include <stdio.h>
#include "webdav_sync.h"

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: probe-save-sync <profile> [game folder, default TOTK]\n"
                            "TOTK_WEBDAV_URL (required), _USER and _PASSWORD name the server.\n");
            return 2;
        }
        mng_webdav_sync_profile(argc > 2 ? argv[2] : "TOTK", argv[1], NULL);
        return 0;
    }
}

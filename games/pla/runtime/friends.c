/* nn::friends overrides: PLA's early init queries the friend list (offline: empty). The SDK's real
   implementation needs the friends service (NPLN-era); the game only needs "no friends" to proceed. */
#include "runtime.h"
#include <string.h>

static uint32_t get_friend_list(int32_t *count_out, void *out, int32_t max, const void *filter) {
    (void)out; (void)filter; (void)max;
    if (count_out) *count_out = 0;
    return 0;
}
static void friend_ctor(void *this) { memset(this, 0, 0x100); }

void *runtime_friends_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn7friends13GetFriendListEPiPNS0_6FriendEiiRKNS0_12FriendFilterE", get_friend_list},
        {"_ZN2nn7friends6FriendC1Ev", friend_ctor},
    };
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++)
        if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}

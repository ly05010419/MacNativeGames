/* The local users of this Mac (runtime/account.c keeps them): one save area each, because the save directory is
   named after the user's id (runtime/fs_host.c). As on a console, a game always plays as the user the system
   preselected: it is chosen before the game starts — here in the settings panel's User page, and it takes effect
   from the next launch, while a running game keeps the user it opened with. At most RUNTIME_USERS_MAX users, as
   many as a console holds. The settings panel lists them, adds one and chooses the next start's user; the store
   itself lives in <TOTK_PROFILE_DIR>/users.txt and survives restarts. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define RUNTIME_USERS_MAX 8   /* a console holds 8 local users */
typedef struct { uint64_t lo, hi; char name[32]; } RuntimeUser;   /* name: UTF-8, at most 31 bytes */

/* Every user in the order they were added; returns how many were written. */
int runtime_users_get(RuntimeUser *out, int max);
/* The user this run opened with (the preselected one). */
int runtime_users_active(RuntimeUser *out);
/* Add a user with this name (trimmed, control characters dropped, at most 31 bytes of UTF-8): its index, or -1
   when the name is empty or the console is full. Its saves start the next time it is chosen. */
int runtime_users_add(const char *name);
/* Make users[index] the user the next launch opens with. 0, or -1 for a bad index. */
int runtime_users_choose(int index);

#ifdef __cplusplus
}
#endif

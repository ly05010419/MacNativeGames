/* nn::pctl (parental controls) · "free communication" with other players (voice chat, online play).
   The host has no pctl service, no networking and never shows a system UI prompt, so the honest answer to every one of these
   is "not available" / "not allowed" -- the same answer a console gives offline and without a parental-controls override.
   Returning 0 here is the real semantics, not a faked success: the question is "may the player talk to others", and this
   port cannot, so the game takes its offline path.

   Measured on Engage (2026-11-13): right after a cutscene, entering a battle map asked IsFreeCommunicationAvailable and the
   game exited. main imports only these three pctl entries (analysis/runtime-layout.json); services.c hosted
   pctl::Initialize but none of them, so the SDK's own implementation ran, connected to the pctl service through sm and the
   host stopped:
     [totk-ipc] sm: service "pctl" not implemented
     [totk] caller sdk+0x34e110  _ZN2nn4pctl6detail3ipc28InitializeIpcSessionIfNeededEv +0x340
     [totk] caller sdk+0x34dcec  _ZN2nn4pctl6detail3ipc10GetServiceEv +0x1c
     [totk] caller sdk+0x34af6c  _ZN2nn4pctl28IsFreeCommunicationAvailableEv +0xc
     [totk] FAIL: IPC service not implemented
   i.e. every battle entry ended the process.

   The upstream nxnative host answers this family the same way and names this very scene
   (dist/work/eng_src/dm/host/hle_misc.c:175-195): IsFreeCommunicationAvailable -> unsigned 0 ("Engage asks on entering a
   battle map (no UI); without this the SDK connects to the pctl service through sm, which the host does not have, and
   aborts"), TryBeginFreeCommunication -> unsigned 0 ("never allowed"), EndFreeCommunication -> void, and
   CheckFreeCommunicationPermission -> unsigned 0 (its own comment: the SDK's version opens the same pctl service). */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

/* One line per entry point, the first time it is asked: enough to see the takeover in a log without a line per frame. */
#define PCTL_ONCE(...) do { static int said; if (!said++) fprintf(stderr, "[totk-pctl] " __VA_ARGS__); } while (0)

static unsigned is_free_communication_available(void) {
    PCTL_ONCE("IsFreeCommunicationAvailable -> 0 (no networking, no system UI)\n");
    return 0;
}

/* bool show_ui (the game passes 0): the host would have nothing to show, and the answer is "no" either way. */
static unsigned try_begin_free_communication(unsigned show_ui) {
    (void)show_ui;
    PCTL_ONCE("TryBeginFreeCommunication -> 0 (never allowed: no networking)\n");
    return 0;
}

static void end_free_communication(void) {
    PCTL_ONCE("EndFreeCommunication (nothing to end: free communication was never allowed)\n");
}

/* The same permission question the two above answer; the SDK's own version opens the pctl service (see the note at the top). */
static unsigned check_free_communication_permission(unsigned show_ui) {
    (void)show_ui;
    PCTL_ONCE("CheckFreeCommunicationPermission -> 0 (no networking, no system UI)\n");
    return 0;
}

void *runtime_pctl_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn4pctl28IsFreeCommunicationAvailableEv", (void *)is_free_communication_available},
        {"_ZN2nn4pctl25TryBeginFreeCommunicationEb", (void *)try_begin_free_communication},
        {"_ZN2nn4pctl20EndFreeCommunicationEv", (void *)end_free_communication},
        {"_ZN2nn4pctl32CheckFreeCommunicationPermissionEb", (void *)check_free_communication_permission}};
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++)
        if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}

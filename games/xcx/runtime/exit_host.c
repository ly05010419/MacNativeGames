/* Nested application exit guards: the native window defers closing until the
   guest leaves its critical section, rather than consulting a console applet. */
#include "runtime.h"
#include <pthread.h>
#include <limits.h>
static pthread_mutex_t exit_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned exit_depth;
static int exit_requested;
void runtime_exit_section_enter(void) {
    pthread_mutex_lock(&exit_lock);
    if (exit_depth == UINT_MAX) runtime_fail("exit section overflow");
    exit_depth++;
    pthread_mutex_unlock(&exit_lock);
}
void runtime_exit_section_leave(void) {
    pthread_mutex_lock(&exit_lock);
    if (!exit_depth) runtime_fail("unbalanced exit section");
    exit_depth--;
    pthread_mutex_unlock(&exit_lock);
}
int runtime_exit_section_active(void) {
    pthread_mutex_lock(&exit_lock);
    int active = exit_depth != 0;
    pthread_mutex_unlock(&exit_lock);
    return active;
}

/* A handling section asks the system to notify the guest before termination.
   Just waiting for Leave would deadlock: the game never sees an exit request.
   Window retries share one notification, even when they arrive concurrently. */
int runtime_exit_request_close(void) {
    pthread_mutex_lock(&exit_lock);
    int active = exit_depth != 0;
    int notify = active && !exit_requested;
    if (notify) exit_requested = 1;
    pthread_mutex_unlock(&exit_lock);
    if (notify) runtime_oe_push_message(4); /* AppletMessage_ExitRequest */
    return !active;
}

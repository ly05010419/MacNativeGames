/* Input as Horizon's hid sysmodule publishes it in the shared memory libnx reads (layout: third_party/libnx/hid_shmem.h):
   one entry per sample pushed into each LIFO (hid_publish.c; tests/hid_probe.c reads it back with libnx's reader). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "keyboard_layout.h"
#include "../third_party/libnx/hid_shmem.h"
void hb_lifo_push(HidCommonLifoHeader *h, void *storage, size_t entry_size, const void *state, size_t state_size, uint64_t number);
/* Touch screen, mouse, keyboard, and the controller as player 1 (full key) and as the handheld console. */
void hb_hid_publish(HidSharedMemory *m, const HbInput *in, uint64_t n);

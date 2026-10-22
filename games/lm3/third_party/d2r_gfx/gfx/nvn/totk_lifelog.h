// TOTK diagnostic DM_NVN_LIFELOG=WxH[:nvnfmt] (e.g. 1600x900:0xa): CPU-side timeline of the textures of that size
// (and format), to follow one resource through the game's frame pipeline. Logged: Initialize / Finalize, texture-pool
// registrations, render targets and texture handles recorded into command buffers, EndRecording / CallCommands of the
// command buffers that recorded them, every queue submission, and (replay side) the texture each draw resolves for a
// handle. Lines carry the time, the thread and the CPU frame (presents so far), so the order across the game's threads
// and submissions can be read off. Off by default; costs one branch per hooked call when off.
#pragma once
#include <stdint.h>
#include "nvn_obj.h"

int totk_life_on(void);
int totk_life_match(const tex_t *t);
void totk_life_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// texture-pool shadow: the serial of the latest matching registration of an id (0 = none or not matching)
void totk_life_reg(int id, const tex_t *t);
uint64_t totk_life_reg_serial(int id);
// command buffers / handles that carried a matching texture, so their EndRecording / CallCommands are logged
void totk_life_cb_mark(const void *cb);
int totk_life_cb_take(const void *cb);
void totk_life_handle_add(uint64_t h);
int totk_life_handle_has(uint64_t h);

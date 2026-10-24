#pragma once
#include <stdint.h>
void *front_metal_layer(void);
void front_frame_presented(void);
uint64_t front_game_res(int docked);
float front_hdr_peak(void);   /* key 8 (Mac): 0 SDR; else the window is EDR and SDR white may brighten up to this multiple */

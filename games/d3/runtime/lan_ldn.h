/* Experimental SDK adapter. "discovery" preserves legacy discovery-only mode;
   explicit MNG_LAN_EXPERIMENTAL=transport enables integrated LDN/UDP testing.
   No Nintendo/EA authentication, TCP, mesh or SystemEvent support implied. */
#pragma once
#include <stdint.h>
void *mng_lan_ldn_override(const char *name, uint64_t title);

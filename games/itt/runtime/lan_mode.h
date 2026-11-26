/* One mode decision shared by BSD and LDN. Runtime replacement can opt into a
   diagnostic default without changing launcher/profile settings. Explicit env
   wins (including "off" and unknown values). Platform builds explicitly select
   their accepted default; standalone/unconfigured compilation remains off. */
#pragma once
#include <stdlib.h>
#include <string.h>
#ifndef MNG_LAN_DEFAULT_TRANSPORT
#define MNG_LAN_DEFAULT_TRANSPORT 0
#endif
#ifndef MNG_LAN_DISCOVERY_TEST_BUILD
#define MNG_LAN_DISCOVERY_TEST_BUILD 0
#endif
#ifndef MNG_LAN_TRANSPORT_TEST_BUILD
#define MNG_LAN_TRANSPORT_TEST_BUILD 0
#endif
enum { MNG_LAN_OFF, MNG_LAN_DISCOVERY, MNG_LAN_TRANSPORT };
static inline int mng_lan_mode(void) {
    const char *mode=getenv("MNG_LAN_EXPERIMENTAL");
    if(mode) return !strcmp(mode,"transport")?MNG_LAN_TRANSPORT:
                    !strcmp(mode,"discovery")?MNG_LAN_DISCOVERY:MNG_LAN_OFF;
    return MNG_LAN_TRANSPORT_TEST_BUILD?MNG_LAN_TRANSPORT:
           MNG_LAN_DISCOVERY_TEST_BUILD?MNG_LAN_DISCOVERY:
           MNG_LAN_DEFAULT_TRANSPORT?MNG_LAN_TRANSPORT:MNG_LAN_OFF;
}

#pragma once
#include <stdint.h>
/* NVN can advertise a fixed maximum (15), including repeated 1x1 tails.
 * Metal truncates that tail; this is different from dropping large top mips. */
static inline unsigned nvn_physical_mips(unsigned width, unsigned height, unsigned depth, unsigned advertised) {
    unsigned extent=width>height?width:height;
    if(depth>extent)extent=depth;
    unsigned count=1;while(extent>1){extent>>=1;count++;}
    if(!advertised)return 1;
    return advertised<count?advertised:count;
}
static inline int nvn_mip_view_range(unsigned full, unsigned metal, unsigned requested,
                                     unsigned count, unsigned *first, unsigned *length) {
    if(!full || !metal || metal>full || requested>=full)return 0;
    unsigned drop=full-metal;
    unsigned start=requested>drop?requested-drop:0;
    if(start>=metal)return 0;
    unsigned available=metal-start;
    *first=start;*length=count && count<available?count:available;
    return *length!=0;
}

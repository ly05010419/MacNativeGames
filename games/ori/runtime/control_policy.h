/* Shared, CPU-only policy for the control menu. Ratios are application presets,
   not undocumented Apple quality enums. Keep inputs at an exact 16:9 ratio. */
#pragma once
#include <stdint.h>
static inline int totk_control_size(unsigned w,unsigned h){return w>=640&&w<=2560&&h>=360&&h<=1440&&w*9==h*16;}
static inline uint64_t totk_control_render(unsigned w,unsigned h,int quality){
 if(!totk_control_size(w,h)||quality<0||quality>3)return 0;
 unsigned numerator[]={1,2,10,1},denominator[]={1,3,17,2};
 unsigned units=(w*numerator[quality]/denominator[quality])/16;
 if(units<40)units=40;
 return (uint64_t)(units*16)|((uint64_t)(units*9)<<32);
}

/* TotK image-coordinate producers, verified against the shipped vertex
 * programs and the actual F2169 scene -> tone -> UI canvas -> present chain.
 * Their fragment texture 0 supplies image coordinates; other bindings are
 * auxiliary samples. UI widgets with CB0/560 are classified separately. */
#pragma once
#include <string.h>
#include "vr_projection.h"
static inline int vr_totk_projection_color_action(uint32_t mask,unsigned attachment,uint32_t blend,int camera,int image_post){
    int action=vr_projection_color_action(mask,attachment,blend,camera,image_post);
    /* Texture dependencies do not declare a pixel-coordinate transform. */
    return action==VR_PROJECTION_INHERIT?VR_PROJECTION_NO_WRITE:action;
}
static inline int vr_totk_image_vertex(const char *key){
    static const char *keys[]={
        "31efeeb02137b8315bdcadb4eaf2bfd6f55ea9b3bf61da95c443be4e90beabeb",
        "78eb7b52167639925be40bbfb151a72759dd1660c98a4d2436b5b9a0b099deec",
        "ca510aa8a2e97ac1fd3df28fffb6b93dd54933e5b813b423895f92dcdf98aadc",
        "1a40e5f5ff257e13293c01b8e293a619d2ae8cdcffd8e4083e04dd7f59fd2273"
    };
    if(!key||strlen(key)<64)return 0;
    for(unsigned i=0;i<sizeof keys/sizeof *keys;i++)if(!memcmp(key,keys[i],64)&&(!key[64]||key[64]=='-'))return 1;
    return 0;
}

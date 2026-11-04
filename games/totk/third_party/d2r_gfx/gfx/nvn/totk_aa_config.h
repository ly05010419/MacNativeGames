/* TOTK's optional spatial AA choices. The selected method and each method's quality are independent.
   No GPU resources or game state: also used by the settings-panel probe. MLOG is the caller's logger. */
#pragma once
#include "totk_smaa_config.h"
enum { TOTK_AA_OFF, TOTK_AA_FXAA, TOTK_AA_SMAA };
static _Atomic int g_fin_fxaa_off, g_fin_smaa, g_fin_fxaa_quality, g_fxaa_quality_choice;

int dm_gfx_fxaa_on(void) { return !atomic_load(&g_fin_fxaa_off); }
int dm_gfx_smaa_on(void) { return atomic_load(&g_fin_smaa); }
int dm_gfx_smaa_quality(void) { return atomic_load(&g_smaa_quality); }
void dm_gfx_smaa_quality_set(int q) { if(q>=SMAA_LOW&&q<=SMAA_ULTRA)atomic_store(&g_smaa_quality,q); }
int dm_gfx_fxaa_quality(void) { return atomic_load(&g_fxaa_quality_choice); }
int dm_gfx_aa_method(void) { return dm_gfx_smaa_on()?TOTK_AA_SMAA:dm_gfx_fxaa_on()?TOTK_AA_FXAA:TOTK_AA_OFF; }
void dm_gfx_fxaa_quality_set(int q) {
    if(q<0||q>1)return;
    atomic_store(&g_fxaa_quality_choice,q);
    atomic_store(&g_fin_fxaa_quality,q&&dm_gfx_aa_method()==TOTK_AA_FXAA);
}
void dm_gfx_fxaa_set(int on) {
    atomic_store(&g_fin_fxaa_off,!on);
    atomic_store(&g_fin_fxaa_quality,on&&!dm_gfx_smaa_on()&&dm_gfx_fxaa_quality());
    MLOG("[fxaa] FXAA %s",on?"on":"off");
}
void dm_gfx_smaa_set(int on) {
    atomic_store(&g_fin_smaa,!!on);
    atomic_store(&g_fin_fxaa_quality,!on&&dm_gfx_fxaa_on()&&dm_gfx_fxaa_quality());
    MLOG("[smaa] %s (replaces FXAA)",on?"on":"off");
}
void dm_gfx_aa_method_set(int method) {
    if(method<TOTK_AA_OFF||method>TOTK_AA_SMAA)return;
    atomic_store(&g_fin_fxaa_off,method==TOTK_AA_OFF);
    atomic_store(&g_fin_smaa,method==TOTK_AA_SMAA);
    atomic_store(&g_fin_fxaa_quality,method==TOTK_AA_FXAA&&dm_gfx_fxaa_quality());
    MLOG("[aa] %s",method==TOTK_AA_OFF?"off":method==TOTK_AA_SMAA?"SMAA":"FXAA");
}

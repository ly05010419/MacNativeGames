/* No Zelda instruction patches or automated save/menu actions apply to Tomodachi. */
#include "runtime.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
_Atomic int runtime_survey_pending;
void runtime_game_patches(Module *m) {
 /* Only physical pool backing needs Darwin's 16 KiB page alignment.
    Keep ordinary graphics allocations at their original alignment: increasing
    those wastes the title's small child heaps. Branch through verified text
    padding, preserving the original allocator, size and ownership. x9 is a
    caller-saved scratch register unused before the allocator call. */
 uint32_t original,replacement;
 const char *pool_alignment=getenv("TOMODACHI_POOL_ALIGNMENT_PATCH");
 if(!pool_alignment || strcmp(pool_alignment,"0")){
  const uint32_t site=0x32a5b0;
  const uint64_t cave=m->text_end;
  uint32_t stub[]={0x2a0103e2, /* mov w2,w1 */
                   0xb94000a9, /* ldr w9,[x5] -- pool flags */
                   0x36400049, /* tbz w9,#8,done -- PHYSICAL */
                   0x531e7422, /* lsl w2,w1,#2 */
                   0};
  memcpy(&original,m->base+site,4);
  if(original!=0x2a0103e2 || (cave&3) || cave>m->ro_off ||
     m->ro_off-cave<sizeof(stub) || cave-site>=0x8000000)
   runtime_fail("Physical pool alignment patch differs from pinned 1.0.0");
  for(unsigned i=0;i<sizeof(stub);i++)
   if(m->base[cave+i])runtime_fail("Physical pool alignment text padding is occupied");
  stub[4]=0x14000000|((uint32_t)(((int64_t)site+4-(int64_t)cave-16)/4)&0x03ffffff);
  memcpy(m->base+cave,stub,sizeof(stub));
  replacement=0x14000000|((uint32_t)((cave-site)/4)&0x03ffffff);
  memcpy(m->base+site,&replacement,4);
  fprintf(stderr,"[tomodachi] Physical NVN pool backing aligned for 16 KiB host pages\n");
 }
 /* Native startup exhausts the 128 KiB MiiBodyBaseModel allocation while
    building model resources (main+0x328f60). Give those owned child heaps
    256 KiB through the game's own heap constructor and allocator. */
 const char *disable=getenv("TOMODACHI_MII_HEAP_PATCH");
 if(disable&&!strcmp(disable,"0"))return;
 memcpy(&original,m->base+0x580434,4);
 if(original!=0x321b2fe0)runtime_fail("Mii model heap instruction differs from pinned 1.0.0");
 replacement=0x321b33e0;memcpy(m->base+0x580434,&replacement,4);
 fprintf(stderr,"[tomodachi] Mii model child heaps: 128 KiB -> 256 KiB\n");
 /* The copied island reaches ClothModel's 32 KiB pools and exhausts one on
    a 3616-byte model-resource allocation. Expand only the two 32 KiB classes
    at their size arguments; preserve the game's allocation and ownership. */
 const uint32_t cloth_offsets[]={0x57b9fc,0x57ba70};
 for(unsigned i=0;i<2;i++){
  memcpy(&original,m->base+cloth_offsets[i],4);
  if(original!=0x52900003)runtime_fail("Cloth model heap instruction differs from pinned 1.0.0");
  replacement=0x52a00023;memcpy(m->base+cloth_offsets[i],&replacement,4);
 }
 fprintf(stderr,"[tomodachi] Cloth model 32 KiB child heaps -> 64 KiB\n");
}
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t ns) { (void)ns; }
int runtime_output_res(unsigned *w,unsigned *h) { (void)w;(void)h;return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
void runtime_survey_init(Module *m) { (void)m; }
int runtime_survey_command(const char *line) { (void)line;return 0; }
void runtime_survey_tick(uint64_t frame) { (void)frame; }
void runtime_survey_cancel_user(void) {}
void runtime_autoload_save_file_opened(const char *path) { (void)path; }
void runtime_autoload_user_input(void) {}

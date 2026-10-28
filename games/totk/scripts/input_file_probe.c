#include "../runtime/runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* TOTK_INPUT_FILE (runtime/input.c) read through its production thread: the lines already in the file at start, a line
   appended in place (the Mac's echo >>), the file replaced by a longer copy of the list (devicectl device copy to an
   iPhone writes a new file) and replaced by a new, shorter list (read from its start). The reader opens the file in a
   constructor, so the probe sets TOTK_INPUT_FILE and runs itself again. Each line is "flag probe <n>". */
uint64_t runtime_now_ns(void){static uint64_t t;return t+=1000000;}
void dm_gfx_shot_at(long frames,const char *path){(void)frames;(void)path;runtime_fail("unexpected screenshot in unit probe");}
void dm_gfx_passdump_next(long n){(void)n;runtime_fail("unexpected pass dump in unit probe");}
void dm_gfx_texdump_next(const char *path){(void)path;runtime_fail("unexpected texture dump in unit probe");}
void runtime_swkbd_selftest(void){runtime_fail("unexpected keyboard self-test in unit probe");}
void runtime_dump_threads(void){runtime_fail("unexpected thread dump in unit probe");}
void runtime_hle_trace_set(int mode){(void)mode;runtime_fail("unexpected host-call trace in unit probe");}
void runtime_frame_step_pin(double ms){(void)ms;runtime_fail("unexpected frame step pin in unit probe");}
void runtime_front_text_test(const char *op,const char *text){(void)op;(void)text;runtime_fail("unexpected keyboard test in unit probe");}
void runtime_settings_toggle(void){runtime_fail("unexpected settings panel in unit probe");}
int runtime_setting_word(const char *name,char *word,size_t size){(void)name;(void)word;(void)size;return 0;}
void runtime_setting_save(const char *name,const char *word,const char *what){(void)name;(void)word;(void)what;}
static _Atomic int flag_value, flags_seen;
void dm_gfx_set_flag(const char *name,int v){if(strcmp(name,"probe"))runtime_fail("unexpected runtime flag in unit probe");atomic_store(&flag_value,v);atomic_fetch_add(&flags_seen,1);}
void dm_gfx_querylog_next(long n){(void)n;runtime_fail("unexpected query log in unit probe");}
void dm_gfx_anomdump(void){runtime_fail("unexpected anomaly dump in unit probe");}
void dm_gfx_window_shot(const char *p){(void)p;runtime_fail("unexpected window shot in unit probe");}
int runtime_survey_command(const char *line){(void)line;return 0;}
void runtime_survey_cancel_user(void){}
void runtime_front_request_close(void){runtime_fail("unexpected window close in unit probe");}
void runtime_gamepad_read(RuntimeGamepadState *out){*out=(RuntimeGamepadState){0};}
void runtime_gamepad_clear(void){}
void runtime_rumble_set(unsigned side,float al,float fl,float ah,float fh){(void)side;(void)al;(void)fl;(void)ah;(void)fh;}
int runtime_handheld_mode(void){return 0;}
_Noreturn void runtime_fail(const char *s){fprintf(stderr,"%s\n",s);exit(1);}

static void write_file(const char *path,const char *mode,const char *text){
 FILE *f=fopen(path,mode);if(!f||fputs(text,f)<0||fclose(f))runtime_fail("probe: cannot write the input file");
}
static void replace_file(const char *path,const char *text){   // a new file under the same name, as devicectl writes it
 char tmp[1100];snprintf(tmp,sizeof tmp,"%s.new",path);write_file(tmp,"w",text);
 if(rename(tmp,path))runtime_fail("probe: cannot replace the input file");
}
static void expect(int value,int seen,const char *what){   // the reader polls every 100 ms
 for(int i=0;i<50&&!(atomic_load(&flag_value)==value&&atomic_load(&flags_seen)==seen);i++)usleep(20000);
 if(atomic_load(&flag_value)!=value||atomic_load(&flags_seen)!=seen){
  fprintf(stderr,"%s: flag probe %d after %d lines, expected %d after %d\n",what,atomic_load(&flag_value),atomic_load(&flags_seen),value,seen);exit(1);
 }
}
int main(int argc,char **argv){
 (void)argc;
 const char *path=getenv("TOTK_INPUT_FILE");
 if(!path){
  char dir[]="/tmp/totk-input-file-probe-XXXXXX",file[1024];
  if(!mkdtemp(dir))runtime_fail("probe: temporary folder");
  snprintf(file,sizeof file,"%s/input.txt",dir);write_file(file,"w","flag probe 1\n");
  setenv("TOTK_INPUT_FILE",file,1);execv(argv[0],argv);runtime_fail("probe: cannot run itself again");
 }
 expect(1,1,"lines present at start");
 write_file(path,"a","flag probe 2\n");expect(2,2,"line appended in place");
 replace_file(path,"flag probe 1\nflag probe 2\nflag probe 3\n");expect(3,3,"file replaced by a longer copy of the list");
 replace_file(path,"flag probe 4\n");expect(4,4,"file replaced by a new, shorter list");
 char dir[1024];snprintf(dir,sizeof dir,"%s",path);*strrchr(dir,'/')=0;unlink(path);rmdir(dir);
 printf("{\"lines_at_start\":true,\"appended_in_place\":true,\"replaced_longer\":true,\"replaced_shorter\":true}\n");
 return 0;
}

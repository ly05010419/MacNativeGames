/* Execute the production priority query and virtual-core grant on synthetic threads. */
#include "../games/totk/runtime/threads.c"
#include <assert.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
_Noreturn void runtime_fail(const char *s){fprintf(stderr,"FAIL %s\n",s);exit(2);}
uint64_t runtime_now_ns(void){mach_timebase_info_data_t t;mach_timebase_info(&t);return (uint64_t)((__uint128_t)mach_absolute_time()*t.numer/t.denom);}
uint64_t runtime_hle_tsd_off;
int runtime_guest_code_pc(uint64_t pc){(void)pc;return 0;}
pthread_key_t runtime_keys[8];
enum { N=128 };
static thr_t nodes[N];
static uint32_t words[N];
static void reset(void){
 memset(g_h,0,sizeof g_h);g_next_lin=1;g_wait=g_core_wait=NULL;
 memset(g_core_owner,0,sizeof g_core_owner);memset(nodes,0,sizeof nodes);memset(words,0,sizeof words);
 for(unsigned i=0;i<N;i++){nodes[i].prio=60;nodes[i].cur_core=-1;nodes[i].mask=1;nodes[i].handle=h_new(O_THREAD,&nodes[i]);}
}
static void waits(unsigned w,unsigned owner){
 words[w]=nodes[owner].handle|WAITERS_BIT;nodes[w].wkind=1;nodes[w].waddr=(uintptr_t)&words[w];
 nodes[w].next=g_wait;g_wait=&nodes[w];
}
int main(int argc,char **argv){
 assert(argc==2);int mode=atoi(argv[1]),chain=mode==1;
 if(mode==2)unsetenv("TOTK_PI_CHAIN");else setenv("TOTK_PI_CHAIN",chain?"1":"0",1);
 unsetenv("TOTK_PI_STEP");unsetenv("TOTK_STARVE");
 pthread_mutex_lock(&G);
 reset();nodes[0].prio=44;nodes[1].prio=50;nodes[2].prio=60;nodes[3].prio=45;
 waits(0,1);waits(1,2); // urgent -> intermediate -> runnable owner
 assert(eff_prio(&nodes[1])==44);assert(eff_prio(&nodes[2])==(chain?44:50));
 // The production scheduler must pick the downstream owner ahead of unrelated work.
 pthread_cond_init(&nodes[2].cv,NULL);pthread_cond_init(&nodes[3].cv,NULL);
 nodes[3].cseq=1;nodes[2].cseq=2;nodes[3].cnext=&nodes[2];g_core_wait=&nodes[3];
 core_grant(0);assert(g_core_owner[0]==&nodes[chain?2:3]);
 pthread_cond_destroy(&nodes[2].cv);pthread_cond_destroy(&nodes[3].cv);
 // A user-space handoff changes the chain immediately; no stale priority cache.
 words[0]=nodes[3].handle|WAITERS_BIT;
 assert(eff_prio(&nodes[2])==50);assert(eff_prio(&nodes[3])==44);
 nodes[0].wkind=0;assert(eff_prio(&nodes[3])==45);
 // Several donors and reverse registration order must choose the best donor.
 reset();nodes[0].prio=20;nodes[1].prio=30;nodes[2].prio=50;nodes[3].prio=60;
 waits(0,2);waits(1,2);waits(2,3);assert(eff_prio(&nodes[3])==(chain?20:50));
 nodes[2].next=&nodes[1];nodes[1].next=&nodes[0];nodes[0].next=NULL;g_wait=&nodes[2];
 assert(eff_prio(&nodes[3])==(chain?20:50));
 // Long chains use constant query stack space, and donor removal restores priority.
 reset();nodes[0].prio=10;
 for(unsigned i=0;i<N-1;i++)waits(i,i+1);
 assert(eff_prio(&nodes[N-1])==(chain?10:60));nodes[0].wkind=0;assert(eff_prio(&nodes[N-1])==60);
 // A cycle is bounded even for an unrelated target and reaches every node in its cycle.
 reset();nodes[0].prio=20;nodes[1].prio=50;nodes[2].prio=60;nodes[3].prio=63;
 waits(0,1);waits(1,2);waits(2,1);
 assert(eff_prio(&nodes[2])==(chain?20:50));assert(eff_prio(&nodes[3])==63);
 // Stale generation and wrong-type owners cannot transfer priority to a new object.
 reset();nodes[0].prio=20;nodes[1].prio=50;waits(0,1);
 unsigned slot=nodes[1].handle&0x7fff;g_hlin[slot]++;
 if(chain)assert(eff_prio(&nodes[1])==50);
 g_hlin[slot]--;g_h[slot].type=O_EVENT_R;
 if(chain)assert(eff_prio(&nodes[1])==50);
 // A condition or address wait is not a mutex ownership edge.
 g_h[slot].type=O_THREAD;nodes[0].wkind=2;assert(eff_prio(&nodes[1])==50);
 nodes[0].wkind=3;assert(eff_prio(&nodes[1])==50);
 // Compare against an independently relaxed priority graph, including arbitrary
 // registration order, cycles, self edges and non-mutex waits.
 if(chain){
  uint32_t random=0x7a9165u;
  for(unsigned round=0;round<512;round++){
   reset();int expected[16],parent[16];
   for(unsigned i=0;i<16;i++){
    random=random*1664525u+1013904223u;nodes[i].prio=(int)((random>>16)%64);expected[i]=nodes[i].prio;
    random=random*1664525u+1013904223u;parent[i]=(random>>16)%19;
    if(parent[i]<16){waits(i,(unsigned)parent[i]);if((random&7)==0)nodes[i].wkind=2;}
   }
   for(unsigned pass=0;pass<16;pass++)for(unsigned i=0;i<16;i++)
    if(nodes[i].wkind==1 && parent[i]<16 && expected[i]<expected[parent[i]])expected[parent[i]]=expected[i];
   for(unsigned i=0;i<16;i++)assert(eff_prio(&nodes[i])==expected[i]);
  }
 }
 // The existing nonzero diagnostic step retains its original direct-only meaning.
 reset();nodes[0].prio=44;nodes[1].prio=50;waits(0,1);g_pi_step=1;
 assert(eff_prio(&nodes[1])==43);
 pthread_mutex_unlock(&G);
 puts(chain?"nested donation and core grant passed":"legacy/default behavior passed");return 0;
}

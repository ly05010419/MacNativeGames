#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void){return NULL;}
module_t *module_of(const void *p){(void)p;return NULL;}
module_t *g_mod;int g_nmod;
void svc_thread_dump(void){}
uint64_t now_ns(void){return bp_now();}
static int cases,bad;
static int ro(NSDictionary *j){
#ifdef NVN_RT_RANGE_RO_IMPL
 return rt_range_bindings_ro(j);
#else
 return 0;
#endif
}
#define E(n,w,a) do{int v=(a);cases++;printf("%s expected %d → got %d\n",n,w,v);bad+=(w)!=v;}while(0)
static NSDictionary *one(NSDictionary *b){return @{ @"route":@"B",@"drv_reads":@{},@"bindings":@[b] };}
int main(void){@autoreleasepool{
 E("explicit sbuf RO",1,ro(one(@{@"kind":@"sbuf",@"msl_type":@"const device _27*",@"rw":@NO})));
 E("missing rw is not proof of a default 0",0,ro(one(@{@"kind":@"sbuf",@"msl_type":@"const device _27*"})));
 E("rw write usage rejected",0,ro(one(@{@"kind":@"sbuf",@"msl_type":@"device _27*",@"rw":@YES})));
 E("device without const rejected",0,ro(one(@{@"kind":@"sbuf",@"msl_type":@"device _27*",@"rw":@NO})));
 E("image with unknown usage rejected",0,ro(one(@{@"kind":@"img",@"msl_type":@"texture2d<float, access::read>"})));
 E("sampled texture explicitly RO",1,ro(one(@{@"kind":@"tex",@"msl_type":@"texture2d<float>"})));
 E("tex with explicit rw rejected",0,ro(one(@{@"kind":@"tex",@"msl_type":@"texture2d<float>",@"rw":@YES})));
 E("constant cbuf with explicit rw rejected",0,ro(one(@{@"kind":@"cbuf",@"msl_type":@"constant _16*",@"rw":@YES})));
 E("rw of unknown type rejected",0,ro(one(@{@"kind":@"tex",@"msl_type":@"texture2d<float>",@"rw":@"false"})));
 E("driver indirect address not extended",0,ro(one(@{@"kind":@"drv0",@"msl_type":@"constant _16*"})));
 E("texture read_write rejected",0,ro(one(@{@"kind":@"tex",@"msl_type":@"texture2d<float, access::read_write>"})));
 E("texture write rejected",0,ro(one(@{@"kind":@"tex",@"msl_type":@"texture2d<float, access::write>"})));
 E("cbuf constant explicitly RO",1,ro(one(@{@"kind":@"cbuf",@"msl_type":@"constant _16*"})));
 E("cbuf of unknown type rejected",0,ro(one(@{@"kind":@"cbuf",@"msl_type":@"device _16*"})));
 E("unknown kind rejected",0,ro(one(@{@"kind":@"opaque",@"msl_type":@"constant _16*"})));
 E("missing bindings rejected",0,ro(@{@"route":@"B"}));
 E("missing driver semantics rejected",0,ro(@{@"route":@"B",@"bindings":@[]}));
 E("driver indirect read present: rejected",0,ro(@{@"route":@"B",@"bindings":@[],@"drv_reads":@{@"0":@YES}}));
 NSString *d=[NSString stringWithFormat:@"%s%s/build/cache/metal_shader_cache/b-%s", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work", DMSH_CHAIN];
 for(NSString *k in @[@"06ece8336c8c242c",@"6d19a794f1206668",@"fa27a102ecd14b3e",@"1bd6a72fa95c6a07"]){NSData *s=[NSData dataWithContentsOfFile:[NSString stringWithFormat:@"%@/%@_b.json",d,k]];if(!s){printf("SKIP: no reflection sample %s/%s_b.json (translated shaders of Super Mario Bros. Wonder in the NSP script's build cache), its check does not run\n",d.UTF8String,k.UTF8String);continue;}NSDictionary *j=[NSJSONSerialization JSONObjectWithData:s options:0 error:nil];E(k.UTF8String,1,ro(j));}
 printf("RT_RANGE_RO cases=%d failed=%d\n",cases,bad);return bad!=0;}}

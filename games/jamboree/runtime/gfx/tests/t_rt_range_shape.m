#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void){return NULL;}
module_t *module_of(const void *p){(void)p;return NULL;}
module_t g_mod[MAX_MOD];int g_nmod;
void svc_thread_dump(void){}
uint64_t now_ns(void){return bp_now();}
static int cases,bad;
#define E(label,w,a) do{int actual=(a);cases++;printf("%s expected %d → got %d\n",label,w,actual);bad+=(w)!=actual;}while(0)
static int shape(const p_set_rt *a,const p_set_rt *b){
#ifdef NVN_RT_RANGE_IMPL
 return rt_range_shape(a,b);
#else
 return rt_same(a,b);
#endif
}
int main(void){@autoreleasepool{
 tex_t c={.magic=MAGIC('T', 'E', 'X', 0), .serial =26},d={.magic=MAGIC('T', 'E', 'X', 0), .serial =29};c.d.width=c.d.height=8;c.d.depth=c.d.samples=1;c.d.levels=4;c.d.target=NVN_TEXTURE_TARGET_2D;c.d.format=NVN_FORMAT_RGBA8;d.d=c.d;d.d.levels=1;d.d.format=NVN_FORMAT_DEPTH32F_STENCIL8;
 view_t cv={.set_levels=1,.set_layers=1,.set_format=1,.set_target=1,.base_level=0,.nlevels=4,.min_layer=0,.nlayers=1,.format=NVN_FORMAT_RGBA8,.target=NVN_TEXTURE_TARGET_2D};view_t dv=cv;dv.nlevels=1;dv.format=d.d.format;
 p_set_rt a={.n=1,.depth=&d,.has_dview=1,.dview=dv};a.color[0]=&c;a.has_cview[0]=1;a.cview[0]=cv;p_set_rt b=a;b.cview[0].nlevels=1;
 E("actual level 0, same layer, only the level count changes",1,shape(&a,&b));
 tex_t small=d;small.d.levels=2;p_set_rt sa=a,sb=b;sa.depth=sb.depth=&small;sa.dview.base_level=sb.dview.base_level=1;E("depth size narrows the color: still split",0,shape(&sa,&sb));
 p_set_rt x=b;x.cview[0].base_level=1;E("level change: still split",0,shape(&a,&x));x=b;x.cview[0].min_layer=1;E("layer change: still split",0,shape(&a,&x));
 x=b;x.cview[0].nlevels=0;E("unknown zero level count: still split",0,shape(&a,&x));x=b;x.cview[0].nlevels=5;E("out-of-range level count: still split",0,shape(&a,&x));
 x=b;x.cview[0].format=NVN_FORMAT_RGBA16F;E("format change: still split",0,shape(&a,&x));x=b;x.cview[0].target=NVN_TEXTURE_TARGET_2D_ARRAY;E("target change: still split",0,shape(&a,&x));
 x=b;x.has_cview[0]=0;E("unknown view: still split",0,shape(&a,&x));x=b;x.cview[0].set_levels=0;E("no explicit level: still split",0,shape(&a,&x));
 x=b;x.cview[0].nlayers=2;E("unknown layer range: still split",0,shape(&a,&x));x=b;x.dview.nlevels=0;E("depth view change: still split",0,shape(&a,&x));x=b;x.depth=NULL;E("depth attachment change: still split",0,shape(&a,&x));
 tex_t other=c;other.serial++;x=b;x.color[0]=&other;E("object serial change: still split",0,shape(&a,&x));other=c;other.d.width=16;x.color[0]=&other;E("same serial, metadata change: still split",0,shape(&a,&x));
 other=c;other.has_layout=1;other.mem=(uint8_t *)1;x=b;x.color[0]=&other;E("packaged with unknown version: still split",0,shape(&a,&x));
 other=c;other.d.target=NVN_TEXTURE_TARGET_2D_ARRAY;x=b;x.color[0]=&other;E("array target: still split",0,shape(&a,&x));other=c;other.d.samples=4;x.color[0]=&other;E("multisampled: still split",0,shape(&a,&x));
 x=b;x.n=2;x.color[1]=&c;E("multiple colors not extended",0,shape(&a,&x));
 printf("RT_RANGE_SHAPE cases=%d failed=%d\n",cases,bad);return bad!=0;
}}

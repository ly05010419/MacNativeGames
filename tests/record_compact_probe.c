/* Compile the real HLE recorder; published handles must be byte-identical. */
#include NVN_HLE_SOURCE
static uint8_t ctl[2][65536];
static void require(int ok,const char *why) { if (!ok) { fprintf(stderr,"FAIL %s\n",why); exit(1); } }
static NVNcommandHandle record(NVNcommandBuffer *c,int compact,size_t *raw,size_t *hw) {
    /* Each process chooses its format before its first draw. */
    setenv("TOTK_RECORD_COMPACT",compact?"1":"0",1);
    h_nvnCommandBufferBeginRecording(c); size_t before=CB(c)->cmd_used;
    for (int k=0;k<1000;k++) {
        h_nvnCommandBufferDrawArrays(c,3,-17,INT32_MAX);
        h_nvnCommandBufferDrawArraysInstanced(c,5,INT32_MIN,23,-71,791);
        h_nvnCommandBufferDrawElementsBaseVertex(c,4,2,901,0xfedcba9876543210ull,-117);
        h_nvnCommandBufferDrawElementsInstanced(c,4,1,910,0xabcdef0987654321ull,-91,-301,17);
        h_nvnCommandBufferDrawArraysIndirect(c,3,0x9988776655443322ull);
        h_nvnCommandBufferDrawElementsIndirect(c,6,2,0xaabbccddeeff0011ull,0x1122334455667788ull);
    }
    h_nvnCommandBufferBindUniformBuffer(c,NVN_SHADER_STAGE_VERTEX,0,0x1234000,512);
    *raw=CB(c)->cmd_used-before; *hw=CB(c)->hw_used;
    return h_nvnCommandBufferEndRecording(c);
}
int main(int argc,char **argv) {
    setenv("TOTK_FLATTEN_FAST","2",1); require(argc==2,"mode"); int compact=atoi(argv[1]); NVNcommandBuffer c;
    h_nvnCommandBufferInitialize(&c,NULL);
    h_nvnCommandBufferAddCommandMemory(&c,NULL,0x100000,1<<24);
    h_nvnCommandBufferAddControlMemory(&c,ctl[0],sizeof ctl[0]);
    size_t raw,hw; NVNcommandHandle handle=record(&c,compact,&raw,&hw);
    #ifdef GAME_BOTW
    handle=botw_handle_copy(handle);
#endif
    const uint8_t *p=(const uint8_t *)(uintptr_t)handle; size_t n=copy_of(handle)->len;
    uint64_t hash=1469598103934665603ull;
    for(size_t j=0;j<n;j++){hash^=p[j];hash*=1099511628211ull;}
    unsigned draws=0; for(const uint8_t *q=p;;){op_hdr h;memcpy(&h,q,8);
        require(!(h.op&NVN_COMPACT_DRAW),"published canonical handle");
        if(h.op==OP_DRAW||h.op==OP_DRAW_IDX)draws++;
        if(h.op==OP_END)break;q+=h.size;}
    require(draws==6000,"all draws retained"); require(copy_of(handle)->nubo==1,"uniform metadata retained");
    printf("%zu %zu %zu %llu\n",raw,hw,n,(unsigned long long)hash); return 0;
}

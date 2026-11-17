#include "../third_party/d2r_gfx/gfx/nvn/tmnt_split_table.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int fail_resize, fail_token;
static size_t token_allocs, releases;
static void *resize_fault(void *p, size_t n) {
    if (fail_resize) return NULL;
    return realloc(p,n);
}
static void *token_fault(size_t count, size_t n) {
    if (fail_token) return NULL;
    void *p=calloc(count,n);
    if (p) token_allocs++;
    return p;
}
static void release_counted(void *p) { if (p) releases++; free(p); }
static void *key(size_t n) { return (void *)(uintptr_t)(n+1); }
int main(void) {
    tmnt_split_table t={0};
    void *saved[129]={0};
    for (size_t i=0;i<128;i++) {
        void *token=NULL;
        assert(tmnt_split_table_get(&t,key(i),resize_fault,token_fault,&token)==0);
        assert(token!=NULL);
        saved[i]=token;
        if (i==15) assert(t.cap==16);
        if (i==16) assert(t.cap==32);
        if (i==32) assert(t.cap==64);
        if (i==64) assert(t.cap==128);
    }
    assert(t.count==128 && token_allocs==128);
    for (size_t i=0;i<128;i++) {
        void *token=NULL;
        assert(tmnt_split_table_get(&t,key(i),resize_fault,token_fault,&token)==0);
        assert(token==saved[i]);
    }
    assert(t.count==128 && token_allocs==128);
    void *out=(void *)1;
    fail_resize=1;
    assert(tmnt_split_table_get(&t,key(128),resize_fault,token_fault,&out)==2);
    assert(t.count==128 && t.cap==128 && out==(void *)1);
    fail_resize=0; fail_token=1;
    assert(tmnt_split_table_get(&t,key(128),resize_fault,token_fault,&out)==3);
    assert(t.count==128 && t.cap==256 && out==(void *)1);
    fail_token=0;
    assert(tmnt_split_table_get(&t,key(128),resize_fault,token_fault,&out)==0);
    assert(t.count==129 && out!=NULL);
    uint8_t control[128]={0};
    assert(tmnt_split_control_position(control,0,63)==0);
    assert(tmnt_split_control_position(control,0,64)==(uintptr_t)control);
    assert(tmnt_split_control_position(control,1,64)==0);
    assert(tmnt_split_control_position(control,64,128)==(uintptr_t)(control+64));
    assert(tmnt_split_control_position(control,SIZE_MAX,SIZE_MAX)==0);
    assert(tmnt_split_control_position(NULL,0,524288)==0);
    assert(tmnt_split_control_position(control,0,524288)==(uintptr_t)control);
    assert(tmnt_split_handle_or_copy(0,saved[0])==(uintptr_t)saved[0]);
    assert(tmnt_split_handle_or_copy((uintptr_t)control,saved[0])==(uintptr_t)control);
    tmnt_split_table_destroy(&t,release_counted);
    assert(t.count==0 && t.cap==0 && t.slots==NULL);
    assert(releases==token_allocs+1);
    puts("tmnt_split_table_test: 129 keys, 16/17/33/65 expansion, reuse, allocation failures, cleanup, control boundaries passed");
}

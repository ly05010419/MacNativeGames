/* Real native ownership, not the last camera to happen to update.
 * 1.4.3 main+6286d4 loads holder=[controller+18], then main+6286e4
 * loads camera=[holder+8]. The same owner supplies its projection at +10.
 * Entries describe that live object relationship, with no frame/age window. */
#pragma once
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
typedef struct {uint64_t controller,holder,camera;} totk_vr_camera_owner;
static pthread_mutex_t totk_vr_owner_mu=PTHREAD_MUTEX_INITIALIZER;
static totk_vr_camera_owner *totk_vr_owners;
static size_t totk_vr_owner_count,totk_vr_owner_capacity;
__attribute__((used,noinline)) static void totk_vr_camera_owner_note(uint64_t controller,uint64_t camera){
    uint64_t holder=0,child=0;
    memcpy(&holder,(const void *)(uintptr_t)(controller+0x18),8);
    if(!holder)return;
    memcpy(&child,(const void *)(uintptr_t)(holder+8),8);
    if(child!=camera)return;
    pthread_mutex_lock(&totk_vr_owner_mu);
    size_t i=0;while(i<totk_vr_owner_count&&totk_vr_owners[i].camera!=camera)i++;
    int added=i==totk_vr_owner_count;
    if(added){
        if(i==totk_vr_owner_capacity){
            size_t capacity=totk_vr_owner_capacity?totk_vr_owner_capacity*2:32;
            void *entries=realloc(totk_vr_owners,capacity*sizeof *totk_vr_owners);
            if(!entries){pthread_mutex_unlock(&totk_vr_owner_mu);return;}
            totk_vr_owners=entries;totk_vr_owner_capacity=capacity;
        }
        totk_vr_owner_count++;
    }
    totk_vr_owners[i]=(totk_vr_camera_owner){controller,holder,camera};
    if(added){uint64_t projection=0;memcpy(&projection,(const void *)(uintptr_t)(holder+0x10),8);
        fprintf(stderr,"[vr-native-owner] controller=%#llx holder=%#llx camera=%#llx projection=%#llx\n",(unsigned long long)controller,(unsigned long long)holder,(unsigned long long)camera,(unsigned long long)projection);}
    pthread_mutex_unlock(&totk_vr_owner_mu);
}
int runtime_vr_camera_owned(uint64_t camera,uint64_t projection){
    totk_vr_camera_owner record={0};
    pthread_mutex_lock(&totk_vr_owner_mu);
    for(size_t i=0;i<totk_vr_owner_count;i++)if(totk_vr_owners[i].camera==camera){record=totk_vr_owners[i];break;}
    pthread_mutex_unlock(&totk_vr_owner_mu);
    uint64_t holder=0,children[2]={0};
    if(!record.controller||!jd_read(record.controller+0x18,&holder,8)||holder!=record.holder||
       !jd_read(holder+8,children,sizeof children)||children[0]!=camera)return 0;
    return !projection||children[1]==projection;
}
extern void vr_cam_owner_hook(void);
__asm__(
".text\n"
".p2align 4\n"
".globl _vr_cam_owner_hook\n"
"_vr_cam_owner_hook:\n"
"sub sp, sp, #688\n"
"stp x0, x1, [sp, #0]\n"
"stp x2, x3, [sp, #16]\n"
"stp x4, x5, [sp, #32]\n"
"stp x6, x7, [sp, #48]\n"
"stp x8, x9, [sp, #64]\n"
"stp x10, x11, [sp, #80]\n"
"stp x12, x13, [sp, #96]\n"
"stp x14, x15, [sp, #112]\n"
"stp x16, x17, [sp, #128]\n"
"stp x18, x30, [sp, #144]\n"
"mrs x16, nzcv\n"
"str x16, [sp, #160]\n"
"stp q0, q1, [sp, #176]\n"
"stp q2, q3, [sp, #208]\n"
"stp q4, q5, [sp, #240]\n"
"stp q6, q7, [sp, #272]\n"
"stp q8, q9, [sp, #304]\n"
"stp q10, q11, [sp, #336]\n"
"stp q12, q13, [sp, #368]\n"
"stp q14, q15, [sp, #400]\n"
"stp q16, q17, [sp, #432]\n"
"stp q18, q19, [sp, #464]\n"
"stp q20, q21, [sp, #496]\n"
"stp q22, q23, [sp, #528]\n"
"stp q24, q25, [sp, #560]\n"
"stp q26, q27, [sp, #592]\n"
"stp q28, q29, [sp, #624]\n"
"stp q30, q31, [sp, #656]\n"
"mov x0, x19\n"
"mov x1, x20\n"
"bl _totk_vr_camera_owner_note\n"
"ldp q0, q1, [sp, #176]\n"
"ldp q2, q3, [sp, #208]\n"
"ldp q4, q5, [sp, #240]\n"
"ldp q6, q7, [sp, #272]\n"
"ldp q8, q9, [sp, #304]\n"
"ldp q10, q11, [sp, #336]\n"
"ldp q12, q13, [sp, #368]\n"
"ldp q14, q15, [sp, #400]\n"
"ldp q16, q17, [sp, #432]\n"
"ldp q18, q19, [sp, #464]\n"
"ldp q20, q21, [sp, #496]\n"
"ldp q22, q23, [sp, #528]\n"
"ldp q24, q25, [sp, #560]\n"
"ldp q26, q27, [sp, #592]\n"
"ldp q28, q29, [sp, #624]\n"
"ldp q30, q31, [sp, #656]\n"
"ldr x16, [sp, #160]\n"
"msr nzcv, x16\n"
"ldp x18, x30, [sp, #144]\n"
"ldp x0, x1, [sp, #0]\n"
"ldp x2, x3, [sp, #16]\n"
"ldp x4, x5, [sp, #32]\n"
"ldp x6, x7, [sp, #48]\n"
"ldp x8, x9, [sp, #64]\n"
"ldp x10, x11, [sp, #80]\n"
"ldp x12, x13, [sp, #96]\n"
"ldp x14, x15, [sp, #112]\n"
"ldp x16, x17, [sp, #128]\n"
"add sp, sp, #688\n"
"b _vr_cam_hook\n"
);

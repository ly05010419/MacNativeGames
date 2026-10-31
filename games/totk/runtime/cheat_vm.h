/* Original interpreter for the documented Atmosphere cheat DATA format.
   This never interprets ARM instructions or supplies a game CPU backend.
   Protocol: Atmosphere-NX/Atmosphere docs/features/cheats.md; no implementation copied.
   Memory access is entirely supplied by the host; execution is bounded and registers
   are isolated per selected entry, so one pack's scratch registers cannot leak to another. */
#ifndef TOTK_CHEAT_VM_H
#define TOTK_CHEAT_VM_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef struct { uint32_t w[4]; unsigned words; size_t jump; } CvInstruction;
typedef struct { CvInstruction *code; size_t count; char error[128]; } CvProgram;
typedef struct {
 void *context;
 int (*read)(void *,uint64_t,unsigned,uint64_t *);
 int (*write)(void *,uint64_t,unsigned,uint64_t);
 uint64_t bases[5],keys;
 unsigned budget;
} CvHost;
enum { CV_OK, CV_BAD_CODE, CV_MEMORY, CV_BUDGET };
static inline unsigned cv_nibble(uint32_t x,unsigned n){return (x>>(4*n))&15;}
static inline int cv_width(unsigned n){return n==1||n==2||n==4||n==8;}
static inline uint64_t cv_mask(unsigned n){return n==8?UINT64_MAX:(UINT64_C(1)<<(8*n))-1;}
static inline unsigned cv_words(uint32_t w){
 unsigned op=w>>28,t=cv_nibble(w,6);
 switch(op){
 case 0:case 1:return cv_width(t)?(t==8?4:3):0;
 case 2:return (w&0x0effffffu)?0:1;
 case 3:return cv_nibble(w,6)==0?2:cv_nibble(w,6)==1?1:0;
 case 4:return 3;
 case 5:return cv_width(t)&&cv_nibble(w,3)<=3?2:0;
 case 6:return cv_width(t)?3:0;
 case 7:return cv_width(t)&&cv_nibble(w,3)<=4?2:0;
 case 8:return 1;
 case 9:return cv_width(t)&&cv_nibble(w,5)<=13&&(cv_nibble(w,5)<10||t==4||t==8)&&cv_nibble(w,2)<=1?(cv_nibble(w,2)?(t==8?3:2):1):0;
 case 10:{unsigned o=cv_nibble(w,2);return cv_width(t)&&o<=5?((o==2||o==4||o==5)?2:1):0;}
 case 12:
  if((w>>24)!=0xc0||!cv_width(cv_nibble(w,5))||cv_nibble(w,4)<1||cv_nibble(w,4)>6)return 0;
  switch(cv_nibble(w,2)){case 0:case 2:return 2;case 1:case 3:case 5:return 1;case 4:return cv_nibble(w,5)==8?3:2;default:return 0;}
 default:return 0;
 }
}
static inline void cv_free(CvProgram *p){free(p->code);memset(p,0,sizeof *p);}
static inline int cv_compile(CvProgram *p,const uint32_t *words,size_t count){
 memset(p,0,sizeof *p);
 if(!count||count>65536){snprintf(p->error,sizeof p->error,"empty or oversized entry");return 0;}
 p->code=calloc(count,sizeof *p->code);if(!p->code)return 0;
 struct {size_t start,alternate;unsigned kind,reg;} stack[64];unsigned depth=0;
 for(size_t at=0;at<count;){
  uint32_t w=words[at];unsigned n=cv_words(w),op=w>>28;size_t i=p->count;
  if(!n||n>count-at){snprintf(p->error,sizeof p->error,"unsupported or truncated opcode at word %zu",at);goto bad;}
  CvInstruction *ins=&p->code[p->count++];memcpy(ins->w,words+at,n*4);ins->words=n;ins->jump=i+1;at+=n;
  int begin=op==1||op==8||(w>>24)==0xc0;
  if(begin||(op==3&&cv_nibble(w,6)==0)){
   if(depth==64){snprintf(p->error,sizeof p->error,"nesting limit");goto bad;}
   stack[depth].start=i;stack[depth].alternate=SIZE_MAX;stack[depth].kind=begin?1:3;stack[depth].reg=cv_nibble(w,4);depth++;
  }else if(op==2){
   if(!depth||stack[depth-1].kind!=1){snprintf(p->error,sizeof p->error,"unmatched conditional end");goto bad;}
   if(cv_nibble(w,6)==1){
    if(stack[depth-1].alternate!=SIZE_MAX){snprintf(p->error,sizeof p->error,"duplicate else");goto bad;}
    p->code[stack[depth-1].start].jump=i+1;stack[depth-1].alternate=i;
   }else{
    depth--;size_t a=stack[depth].alternate;
    p->code[a==SIZE_MAX?stack[depth].start:a].jump=i+1;
   }
  }else if(op==3){
   if(!depth||stack[depth-1].kind!=3||stack[depth-1].reg!=cv_nibble(w,4)){snprintf(p->error,sizeof p->error,"unmatched loop");goto bad;}
   depth--;ins->jump=stack[depth].start+1;p->code[stack[depth].start].jump=i+1;
  }
 }
 if(depth){snprintf(p->error,sizeof p->error,"unterminated block");goto bad;}
 return 1;
bad:free(p->code);p->code=NULL;p->count=0;return 0;
}
static inline int cv_compare(unsigned c,uint64_t a,uint64_t b){
 switch(c){case 1:return a>b;case 2:return a>=b;case 3:return a<b;case 4:return a<=b;case 5:return a==b;case 6:return a!=b;default:return 0;}
}
static inline uint64_t cv_arithmetic(unsigned op,unsigned width,uint64_t a,uint64_t b){
 uint64_t mask=cv_mask(width);a&=mask;b&=mask;
 switch(op){
 case 0:return (a+b)&mask;case 1:return (a-b)&mask;case 2:return (a*b)&mask;
 case 3:return b>=width*8?0:(a<<b)&mask;case 4:return b>=width*8?0:a>>b;
 case 5:return a&b;case 6:return a|b;case 7:return (~a)&mask;case 8:return a^b;case 9:return a;
 default:
  if(width==4){uint32_t x=(uint32_t)a,y=(uint32_t)b,z;float u,v,r;memcpy(&u,&x,4);memcpy(&v,&y,4);r=op==10?u+v:op==11?u-v:op==12?u*v:u/v;memcpy(&z,&r,4);return z;}
  if(width==8){double u,v,r;uint64_t z;memcpy(&u,&a,8);memcpy(&v,&b,8);r=op==10?u+v:op==11?u-v:op==12?u*v:u/v;memcpy(&z,&r,8);return z;}
  return 0;
 }
}
static inline int cv_execute(const CvProgram *p,CvHost *h){
 uint64_t r[16]={0};unsigned budget=h->budget?h->budget:100000;
 if(!p->code||!h->read||!h->write)return CV_BAD_CODE;
 for(size_t ip=0;ip<p->count;){
  if(!budget--)return CV_BUDGET;
  const CvInstruction *in=&p->code[ip++];uint32_t w=in->w[0];unsigned op=w>>28,t=cv_nibble(w,6),m=cv_nibble(w,5),d=cv_nibble(w,4);
  uint64_t a=0,v=0,off=((uint64_t)(w&255)<<32)|in->w[1];unsigned type=0;
  switch(op){
  case 0:
   if(m>4)return CV_BAD_CODE;
   a=h->bases[m]+r[d]+off;v=t==8?((uint64_t)in->w[2]<<32)|in->w[3]:in->w[2];
   if(!h->write(h->context,a,t,v&cv_mask(t)))return CV_MEMORY;break;
  case 1:
   if(m>4||cv_nibble(w,3)>1)return CV_BAD_CODE;
   a=h->bases[m]+off+(cv_nibble(w,3)?r[cv_nibble(w,2)]:0);
   if(!h->read(h->context,a,t,&v))return CV_MEMORY;
   a=t==8?((uint64_t)in->w[2]<<32)|in->w[3]:in->w[2];
   if(!cv_compare(d,v&cv_mask(t),a&cv_mask(t)))ip=in->jump;break;
  case 2:if(cv_nibble(w,6))ip=in->jump;break;
  case 3:
   if(cv_nibble(w,6)==0){r[d]=in->w[1];if(!r[d])ip=in->jump;}
   else if(r[d]&&--r[d])ip=in->jump;
   break;
  case 4:r[d]=((uint64_t)in->w[1]<<32)|in->w[2];break;
  case 5:
   type=cv_nibble(w,3);
   if(m>4)return CV_BAD_CODE;
   a=off+(type==0?h->bases[m]:type==1?r[d]:type==2?r[cv_nibble(w,2)]:h->bases[m]+r[cv_nibble(w,2)]);
   if(!h->read(h->context,a,t,&r[d]))return CV_MEMORY;r[d]&=cv_mask(t);break;
  case 6:
   if(cv_nibble(w,2)>1||cv_nibble(w,3)>1)return CV_BAD_CODE;
   a=r[d]+(cv_nibble(w,2)?r[cv_nibble(w,1)]:0);v=((uint64_t)in->w[1]<<32)|in->w[2];
   if(!h->write(h->context,a,t,v&cv_mask(t)))return CV_MEMORY;
   if(cv_nibble(w,3))r[d]+=t;break;
  case 7:r[d]=cv_arithmetic(cv_nibble(w,3),t,r[d],in->w[1]);break;
  case 8:if((h->keys&(w&0x0fffffffu))!=(w&0x0fffffffu))ip=in->jump;break;
  case 9:
   v=cv_nibble(w,2)?(t==8?((uint64_t)in->w[1]<<32)|in->w[2]:in->w[1]):r[cv_nibble(w,1)];
   r[d]=cv_arithmetic(m,t,r[cv_nibble(w,3)],v);break;
  case 10:
   type=cv_nibble(w,2);v=((uint64_t)(w&15)<<32)|in->w[1];a=r[d];
   if(type>=3&&cv_nibble(w,1)>4)return CV_BAD_CODE;
   if(type==1)a+=r[cv_nibble(w,1)];else if(type==2)a+=v;
   else if(type==3)a+=h->bases[cv_nibble(w,1)];else if(type==4)a=h->bases[cv_nibble(w,1)]+v;else if(type==5)a+=h->bases[cv_nibble(w,1)]+v;
   if(!h->write(h->context,a,t,r[m]&cv_mask(t)))return CV_MEMORY;
   if(cv_nibble(w,3))r[d]+=t;break;
  case 12:{
   unsigned width=cv_nibble(w,5),cmp=cv_nibble(w,4),src=cv_nibble(w,3),kind=cv_nibble(w,2),base=cv_nibble(w,1),other=w&15;
   off=((uint64_t)other<<32)|in->w[1];
   if((kind==0||kind==1)&&base>4)return CV_BAD_CODE;
   if(kind==4)v=width==8?((uint64_t)in->w[1]<<32)|in->w[2]:in->w[1];
   else if(kind==5)v=r[base];
   else{a=kind==0?h->bases[base]+off:kind==1?h->bases[base]+r[other]:kind==2?r[base]+off:r[base]+r[other];if(!h->read(h->context,a,width,&v))return CV_MEMORY;}
   if(!cv_compare(cmp,r[src]&cv_mask(width),v&cv_mask(width)))ip=in->jump;break;
  }
  default:return CV_BAD_CODE;
  }
 }
 return CV_OK;
}
#endif

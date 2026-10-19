#include "audio_effects.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
_Noreturn void runtime_fail(const char *s){fprintf(stderr,"%s\n",s);exit(2);}
static void read_exact(FILE *f,void *p,size_t n){if(fread(p,1,n,f)!=n)runtime_fail("short reference file");}
static int read_int(FILE *f){int n;read_exact(f,&n,4);return n;}
int main(int argc,char **argv){
    if(argc!=2)return 2;FILE *f=fopen(argv[1],"rb");if(!f)return 2;
    if(read_int(f)!=0x41454658)runtime_fail("reference magic");int cases=read_int(f);unsigned long count=0;float max_error=0;
    for(int t=0;t<cases;t++){
        int type=read_int(f),revision=read_int(f),channels=read_int(f),frames=read_int(f),blocks=read_int(f);AudioEffectDsp *s=NULL;
        if(channels<1||channels>6||frames>1024)runtime_fail("reference dimensions");
        float input[6][1024],output[6][1024],*ip[6],*op[6];for(int c=0;c<channels;c++){int i=c;if(channels==6 && ((type==3 && revision<11)||(type==4 && revision>=11))){static const int map[6]={0,1,4,5,2,3};i=map[c];}ip[c]=input[i];op[c]=output[i];}
        unsigned char previous[160]={0};
        for(int b=0;b<blocks;b++){
            int enabled=read_int(f),reset=read_int(f);unsigned char param[160];read_exact(f,param,160);
            if(reset){audio_effect_free(s);s=NULL;}
            if(enabled){if(!s)s=audio_effect_create(type,param,revision);else if(memcmp(param,previous,160))audio_effect_update(s,param);}
            memcpy(previous,param,160);
            for(int c=0;c<channels;c++)read_exact(f,input[c],(size_t)frames*4);
            if(enabled)audio_effect_process(s,ip,op,frames);else for(int c=0;c<channels;c++)memcpy(output[c],input[c],(size_t)frames*4);
            for(int c=0;c<channels;c++)for(int k=0;k<frames;k++){
                float want;read_exact(f,&want,4);float diff=fabsf(output[c][k]-want),limit=2e-5f+fabsf(want)*2e-5f;
                if(!isfinite(output[c][k])||diff>limit){fprintf(stderr,"type%d channels%d block%d sample%d channel%d: got %.9g want %.9g diff %.9g\n",type,channels,b,k,c,output[c][k],want,diff);return 1;}
                if(diff>max_error)max_error=diff;count++;
            }
            if(type==9){float want[8],actual[8];read_exact(f,want,sizeof want);audio_effect_statistics(s,actual,sizeof actual);
                for(int k=0;k<8;k++)if(fabsf(actual[k]-want[k])>2e-5f+fabsf(want[k])*2e-5f){fprintf(stderr,"compressor statistics block%d item%d got%.9g want%.9g\n",b,k,actual[k],want[k]);return 1;}}
        }
        audio_effect_free(s);printf("pass type%d channels%d\n",type,channels);
    }
    if(fgetc(f)!=EOF)runtime_fail("extra reference bytes");fclose(f);
    printf("{\"status\":\"pass\",\"samples\":%lu,\"max_error\":%.9g}\n",count,max_error);return 0;
}

#include "../games/totk/runtime/switch2_udp.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    unsigned char p[44]={0}; memcpy(p,"S2B1",4); Switch2UDPState s={0};
    const unsigned bits[]={2,3,0,1,11,10,22,6,23,7,9,8,19,17,18,16};
    for(unsigned i=0;i<16;i++) {
        uint32_t b=1u<<bits[i]; for(int j=0;j<4;j++) p[8+j]=(unsigned char)(b>>(8*j));
        assert(s2udp_decode(p,44,&s) && s.buttons==(1ull<<i));
    }
    memset(p+8,0,4); float axes[]={-1,.5f,2,-2};
    for(int i=0;i<4;i++) memcpy(p+12+i*4,&axes[i],4);
    assert(s2udp_decode(p,44,&s));
    assert(s.axes[0]==-1 && s.axes[1]==.5f && s.axes[2]==1 && s.axes[3]==-1);
    p[28]=255;p[29]=128; assert(s2udp_decode(p,44,&s) && s.buttons==((1u<<8)|(1u<<9)));
    float nan=NAN;memcpy(p+12,&nan,4);Switch2UDPState before=s;
    assert(!s2udp_decode(p,44,&s) && !memcmp(&before,&s,sizeof s));
    assert(!s2udp_decode(p,43,&s));p[0]='X';assert(!s2udp_decode(p,44,&s));
    memset(p,0,sizeof p);memcpy(p,"S2B1",4);
    p[32]=0x00;p[33]=0x80;p[34]=0xff;p[35]=0x7f;p[42]=0x00;p[43]=0x10;
    assert(s2udp_decode(p,44,&s));assert(s.gyro[0]==-32768 && s.gyro[1]==32767 && s.accel[2]==4096);
    double acc[3],gyro[3];s2udp_motion(&s,.070,acc,gyro);
    assert(acc[0]==0 && acc[1]==0 && acc[2]==-1 && gyro[0]<0 && gyro[1]>0 && gyro[2]==0);
    s=(Switch2UDPState){.gyro={1000,0,0},.accel={0,4096,0}};
    s2udp_motion(&s,.061,acc,gyro); /* measured nose-up: pitch X, gravity toward -Y */
    assert(acc[1]==-1 && gyro[0]>1 && gyro[1]==0 && gyro[2]==0);
    unsigned char motor[6];s2udp_rumble_packet(motor,.5f,2);
    assert(!memcmp(motor,"S2R1",4) && motor[4]==128 && motor[5]==255);
    s2udp_rumble_packet(motor,-1,NAN);assert(!motor[4] && !motor[5]);
    assert(s2udp_gyro_scale("0.0596\n",.07)==.0596);
    assert(s2udp_gyro_scale("nan",.07)==.07 && s2udp_gyro_scale("0.1bad",.07)==.07);
    float bands[2][4]={{.25f,160,.5f,320},{.75f,80,1,400}};unsigned char hd[36];s2udp_hd_packet(hd,bands);
    assert(!memcmp(hd,"S2H1",4));assert(s2udp_u32(hd+8)==0x43200000); /* 160 Hz */
    assert(s2udp_u32(hd+20)==0x3f400000 && s2udp_u32(hd+32)==0x43c80000); /* independent right */
    puts("S2UDP PASS");
}

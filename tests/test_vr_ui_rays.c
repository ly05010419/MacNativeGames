#include "../zeldaVR/gfx/vr_ui_rays.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

static void turn(float yaw,float roll,float r[9]) {
    float y[9]={cosf(yaw),0,sinf(yaw),0,1,0,-sinf(yaw),0,cosf(yaw)};
    float z[9]={cosf(roll),-sinf(roll),0,sinf(roll),cosf(roll),0,0,0,1};
    for(int i=0;i<3;i++)for(int j=0;j<3;j++){r[3*i+j]=0;for(int k=0;k<3;k++)r[3*i+j]+=z[3*i+k]*y[3*k+j];}
}
int main(void) {
    const float ui[16]={1.54545605f,0,0,0,0,2.74747753f,0,0,0,0,-1.0002f,-2.0002f,0,0,-1,0};
    /* Actual F10794 weapon background: its original UI transform maps all
       four corners exactly onto the eye, despite the anamorphic pixel grid. */
    float background[140]={0};memcpy(background,ui,sizeof ui);
    background[16]=background[21]=background[26]=1;background[27]=-989.0919189453125f;
    background[132]=1280;background[133]=720;background[134]=-640;background[135]=360;
    assert(vr_ui_rays_fullscreen(background,0));
    assert(!vr_ui_rays_fullscreen(background,.14f));
    background[132]=640;assert(!vr_ui_rays_fullscreen(background,0));
    background[132]=1280;background[19]=1;assert(!vr_ui_rays_fullscreen(background,0));
    float fov[2][4]={{-.942478f,.698132f,.767945f,-.959931f},{-.698132f,.942478f,.767945f,-.959931f}};
    for(int e=0;e<2;e++)for(int j=0;j<4;j++)fov[e][j]=atanf(tanf(fov[e][j])*1.06f);
    const float headset[2][4]={{-.942478f,.698132f,.767945f,-.959931f},{-.698132f,.942478f,.767945f,-.959931f}};
    assert(vr_projection_eye_grid(fov[0],headset[0],1.06f)&&vr_projection_eye_grid(fov[1],headset[1],1.06f));
    assert(!vr_projection_eye_grid(fov[0],headset[1],1.06f));
    const float inventory[4]={-.391815603f,.102929696f,.259297699f,-.264297694f};
    assert(!vr_projection_eye_grid(inventory,headset[0],1.06f)&&!vr_projection_eye_grid(inventory,headset[1],1.06f));
    float rotation[2][9],target[9];turn(.021f,.013f,rotation[0]);turn(-.027f,-.019f,rotation[1]);turn(.22f,.11f,target);
    float baseline_error=0;
    for(int iy=-1;iy<=1;iy++)for(int ix=-1;ix<=1;ix++){
        float rays[2][3];
        for(unsigned e=0;e<2;e++){
            float shift=e?-.142708f:.142708f;vr_ui_rays map;
            assert(vr_ui_rays_make(ui,fov[e],rotation[e],rotation[0],shift,&map));
            float model[16];memcpy(model,ui,sizeof model);
            /* The bound widget/font matrix includes translation and depth. */
            for(int i=0;i<4;i++)model[4*i+3]+=ui[4*i+2]*-3+ui[4*i]*ix*.4f+ui[4*i+1]*iy*.2f;
            float patched[16];vr_ui_rays_matrix(&map,model,patched);
            if(getenv("TOTK_PROBE_NEGATIVE"))memcpy(patched,model,sizeof patched);
            float clip[4];for(int i=0;i<4;i++)clip[i]=patched[4*i+3];
            clip[0]+=shift*clip[3];
            assert(clip[3]>0&&fabsf(clip[2])<=clip[3]);
            float l=tanf(fov[e][0]),r=tanf(fov[e][1]),u=tanf(fov[e][2]),d=tanf(fov[e][3]);
            rays[e][0]=(l+r)*.5f+(r-l)*.5f*clip[0]/clip[3];
            rays[e][1]=(u+d)*.5f+(u-d)*.5f*clip[1]/clip[3];rays[e][2]=-1;
            float delta[9];for(int i=0;i<3;i++)for(int j=0;j<3;j++)delta[3*i+j]=vr_dot3(target+3*i,rotation[e]+3*j);
            vr_rotation_apply(delta,rays[e]);for(int j=0;j<2;j++)rays[e][j]/=-rays[e][2];
            double vp[4]={e*960,0,960,1080};uint32_t sc[4]={0,0,1920,1080},out[4];
            assert(vr_ui_rays_scissor(&map,vp,sc,out));assert(out[0]==e*960&&out[1]==0&&out[2]==960&&out[3]==1080);
            uint32_t partial[4]={e*960+300,400,100,80};assert(vr_ui_rays_scissor(&map,vp,partial,out));
            assert(out[0]>=e*960&&out[0]+out[2]<=(e+1)*960&&out[1]+out[3]<=1080);
        }
        float error=fmaxf(fabsf(rays[0][0]-rays[1][0]),fabsf(rays[0][1]-rays[1][1]));baseline_error=fmaxf(error,baseline_error);
        if(error>2e-5f){fprintf(stderr,"HUD binocular ray mismatch: %.7f\n",error);return 1;}
    }
    /* TotK F2169 CB0: orthographic 1920x1080 UI, fitted into a portrait
       viewport. Unequal eye projections and source rotations must still
       produce the same final ray, without changing the authored HUD size. */
    float reference[16]={0};reference[0]=.65f;reference[5]=.70f;reference[10]=reference[14]=-1;
    double full[4]={0,0,1280,1380},authored[4]={0,300,1280,780};
    for(int iy=-1;iy<=1;iy++)for(int ix=-1;ix<=1;ix++){
        float result[2][2];
        for(int e=0;e<2;e++){
            vr_ui_rays fitted;assert(vr_ui_rays_make(reference,fov[e],rotation[e],rotation[0],0,&fitted));
            assert(vr_ui_rays_viewport(&fitted,authored,full));
            float orthographic[16]={.001041666721f,0,0,0,0,.001851851819f,0,0,0,0,-.006666666828f,-1,0,0,0,1};
            orthographic[3]=ix*.5f;orthographic[7]=iy*.4f;
            float patched[16];vr_ui_rays_matrix(&fitted,orthographic,patched);
            assert(patched[15]>0);
            float l=tanf(fov[e][0]),r=tanf(fov[e][1]),u=tanf(fov[e][2]),d=tanf(fov[e][3]);
            float ray[3]={(l+r)*.5f+(r-l)*.5f*patched[3]/patched[15],(u+d)*.5f+(u-d)*.5f*patched[7]/patched[15],-1};
            float delta[9];for(int i=0;i<3;i++)for(int j=0;j<3;j++)delta[3*i+j]=vr_dot3(target+3*i,rotation[e]+3*j);
            vr_rotation_apply(delta,ray);for(int j=0;j<2;j++)result[e][j]=ray[j]/-ray[2];
        }
        float expected[3]={ix*.5f/reference[0],iy*.4f*(780.f/1380.f)/reference[5],-1},delta[9];
        for(int i=0;i<3;i++)for(int j=0;j<3;j++)delta[3*i+j]=vr_dot3(target+3*i,rotation[0]+3*j);
        vr_rotation_apply(delta,expected);
        for(int j=0;j<2;j++){
            assert(fabsf(result[0][j]-result[1][j])<2e-5f);
            assert(fabsf(result[0][j]-expected[j]/-expected[2])<2e-5f);
        }
    }
    vr_ui_rays map;float invalid[9]={0};assert(!vr_ui_rays_make(ui,fov[0],invalid,rotation[0],.1f,&map));
    printf("HUD asymmetric FOV, native shift, unequal source rotations, clipping and final binocular rays passed (max %.8f)\n",baseline_error);
}

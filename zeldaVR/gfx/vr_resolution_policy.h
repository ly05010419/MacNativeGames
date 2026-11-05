#pragma once
enum { VR_STREAM_EYE_WIDTH=2048,VR_STREAM_EYE_HEIGHT=2208,VR_RENDER_SIZE_COUNT=8 };
static const int vr_render_sizes[VR_RENDER_SIZE_COUNT][2]={
    {1280,1380},{1536,1656},{1792,1932},{2048,2208},{2560,2760},{3072,3312},{3584,3864},{4096,4416}
};
static inline int vr_render_size_valid(int w,int h) {
    for(int i=0;i<VR_RENDER_SIZE_COUNT;i++)if(w==vr_render_sizes[i][0]&&h==vr_render_sizes[i][1])return 1;
    return 0;
}

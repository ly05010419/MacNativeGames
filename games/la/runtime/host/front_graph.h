// The corner box's graph (front_hud.m DmFrameGraph): frames per second from each frame's time, 0 to 60 as in Tears of the
// Kingdom's FPS graph. A frame faster than the top of the scale is drawn at the top. Pure, for the settings UI test.
#pragma once
#define DM_GRAPH_TOP_FPS 60.0
static inline double dm_graph_fps(double frame_ms, double top) {
    if (!(frame_ms > 0)) return 0;
    double fps = 1000.0 / frame_ms;
    return fps > top ? top : fps;
}

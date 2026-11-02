// TOTK VR: the client side of the BasaltVR streamer's runtime control socket (protocol v9, MIT,
// github.com/frenchbaguetteman/BasaltVR runtime/pe/basalt_control_protocol.h). `basalt-streamer --wired --runtime <file>`
// writes "<port> <token> <width> <height>" to <file>, carries our per-eye images to the ALVR client on a Quest and sends
// the headset's pose back. Pure C, no Metal: the compositor (totk_vr.m) renders, this file only talks.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define VR_LINK_PROTOCOL 9u
#define VR_LINK_FORMAT_BGRA8 87u   // DXGI_FORMAT_B8G8R8A8_UNORM, the streamer takes BGRA IOSurfaces only

typedef struct {
    uint64_t ts;    // Quest tracking timestamp (ns): echoed in vr_link_frame_ready for the frame rendered with this pose
    float q[4];     // orientation xyzw, OpenXR local space (y up, -z forward)
    float p[3];     // position, metres
} vr_pose;

typedef struct {
    float ipd;      // metres
    float fov[2][4];// per eye (left, right): angle left, right, up, down in radians (OpenXR XrFovf: left and down < 0)
} vr_views;

typedef struct {
    uint64_t swapchain;
    uint32_t image;
    int32_t rect[4];// x, y, width, height inside the image
    vr_pose pose;   // the eye pose the view was rendered with
    float fov[4];
} vr_view_submit;

// Reads the control file, connects and says hello. 0 on success; otherwise -1 and the reason in err.
int vr_link_open(const char *control_path, const char *renderer, char *err, size_t n);
void vr_link_close(void);
int vr_link_alive(void);
void vr_link_view_size(int *w, int *h);           // the per-eye size the streamer encodes (from the control file)
int vr_link_views(vr_views *out);                 // 1 once the headset's calibration (VIEW_CONFIG) arrived
// The newest head pose; waits up to timeout_ms for one newer than after_ts. 1 when out holds a pose newer than after_ts.
int vr_link_wait_pose(vr_pose *out, uint64_t after_ts, int timeout_ms);
// Registers images (global IOSurface ids) and waits for the acknowledgement. 0 on success.
int vr_link_create_swapchain(uint64_t id, uint32_t w, uint32_t h, uint32_t format, int count, const uint32_t *surfaces);
// Hands a finished frame (GPU done) to the streamer. 0 on success.
int vr_link_frame_ready(uint64_t frame, int64_t display_time, uint64_t tracking_ts, int nviews, const vr_view_submit *v);
// Frame numbers the streamer finished reading since the last call (oldest first); returns how many were written.
int vr_link_take_consumed(uint64_t *out, int max);
/* Waits up to timeout_ms for a FRAME_CONSUMED not yet taken; 1 when one is there (or came), 0 on timeout or a closed link. */
int vr_link_wait_consumed(int timeout_ms);
/* What the Quest itself reports it showed (BasaltVR streamer, local change: one packet per frame it shows, counted each
   second): 1 with the last second's frames and their average latency, 0 when there is no report from the last 2.5 s
   (the streamer is older, or not connected). shown == 0: the Quest shows no picture. */
int vr_link_headset(float *shown, float *latency_ms);
void vr_link_stats(char *out, size_t n);
// The Quest Touch controllers (BUTTON_STATE, message 11: the ALVR client's inputs by path id, as BasaltVR's own runtime
// maps them in runtime/pe/input.cpp kTouchSources). Buttons by VR_PAD_*; sticks -1..1 (y up), triggers and grips 0..1.
enum { VR_PAD_A = 1, VR_PAD_B = 2, VR_PAD_X = 4, VR_PAD_Y = 8, VR_PAD_MENU = 16, VR_PAD_LSTICK = 32, VR_PAD_RSTICK = 64 };
typedef struct { uint32_t buttons; float lx, ly, rx, ry, ltrig, rtrig, lgrip, rgrip; uint64_t at_ns; } vr_controls;
int vr_link_controls(vr_controls *out);
// The controllers' poses (CONTROLLER_MOTION, message 10), in the head pose's space: orientation xyzw, position (m),
// linear (m/s) and angular (rad/s) velocity. have: a pose arrived; ts: its Quest tracking time.
typedef struct { float q[4], p[3], v[3], w[3]; uint64_t ts; int have; } vr_hand;
void vr_link_hands(vr_hand out[2]);              // 0 left, 1 right
int vr_link_pose(vr_pose *out);                  // the newest head pose, 1 once there is one
int vr_link_send_haptic(int hand, uint64_t duration_ns, float frequency, float amplitude);   // a controller's vibration (0 left, 1 right); 0 on success           // 1 once a controller input arrived (at_ns: the last one, CLOCK_MONOTONIC)
// The game's own audio (local streamer change, message 100): 48 kHz stereo PCM16 interleaved, frames <= 1000. 0 on success.
int vr_link_send_audio(const int16_t *pcm, int frames);

/* scenecap capture module: game-independent scene recording into a .nvs package.
 *
 * The replay layer (d2r_gfx's NVN -> Metal translation) calls the hooks below while it decides what to
 * draw; this module records passes, pipelines, draws, argument tables and snapshots of every texture and
 * buffer they reference, then writes the whole scene as one self-contained .nvs package (see
 * tools/scenecap/README.md for the container format and the manifest contract).
 *
 * The module is plain C11: no Objective-C, no Metal and no libc++. The replay layer supplies the host
 * callbacks for anything GPU flavoured (texture metadata, texture/buffer bytes, MSL source and reflection).
 *
 * Threading: every hook must be called from the replay/decision thread. The hooks only allocate and copy;
 * nothing is written to disk until scenecap_frame_begin() reaches the end of the capture window.
 */
#ifndef SCENECAP_CAPTURE_H
#define SCENECAP_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever scenecap_state / scenecap_pipeline / scenecap_draw change; the glue compiles against this. */
#define SCENECAP_CAPTURE_API 3

typedef enum {
    SCENECAP_PASS_RENDER = 0,
    SCENECAP_PASS_COMPUTE = 1,
    SCENECAP_PASS_BLIT = 2
} scenecap_pass_kind;

typedef enum {
    SCENECAP_LOAD_LOAD = 0,
    SCENECAP_LOAD_CLEAR = 1,
    SCENECAP_LOAD_DONTCARE = 2
} scenecap_load_action;

typedef enum {
    SCENECAP_STORE_STORE = 0,
    SCENECAP_STORE_DONTCARE = 1
} scenecap_store_action;

typedef enum {
    SCENECAP_STAGE_VERTEX = 0,
    SCENECAP_STAGE_FRAGMENT = 1,
    SCENECAP_STAGE_KERNEL = 2,
    SCENECAP_STAGE_OBJECT = 3,
    SCENECAP_STAGE_MESH = 4,
    SCENECAP_STAGE_TILE = 5
} scenecap_stage;

typedef enum {
    SCENECAP_INDEX_NONE = 0,
    SCENECAP_INDEX_UINT8 = 1, /* widened to uint16 by the replay layer */
    SCENECAP_INDEX_UINT16 = 2,
    SCENECAP_INDEX_UINT32 = 3
} scenecap_index_type;

typedef enum {
    SCENECAP_PRIM_POINT = 0,
    SCENECAP_PRIM_LINE = 1,
    SCENECAP_PRIM_LINE_STRIP = 2,
    SCENECAP_PRIM_TRIANGLE = 3,
    SCENECAP_PRIM_TRIANGLE_STRIP = 4
} scenecap_primitive;

typedef enum {
    SCENECAP_BIND_VERTEX_BUFFER = 0,
    SCENECAP_BIND_TEXTURE = 1,
    SCENECAP_BIND_SAMPLER = 2,
    SCENECAP_BIND_UNIFORM_BUFFER = 3,
    SCENECAP_BIND_STORAGE_BUFFER = 4,
    SCENECAP_BIND_STORAGE_IMAGE = 5,
    SCENECAP_BIND_FRAGMENT_BUFFER = 6
} scenecap_binding_kind;

/* Normalised blend factors, independent of NVN's numbering. */
typedef enum {
    SCENECAP_BLEND_ZERO = 0,
    SCENECAP_BLEND_ONE = 1,
    SCENECAP_BLEND_SOURCE_COLOR = 2,
    SCENECAP_BLEND_ONE_MINUS_SOURCE_COLOR = 3,
    SCENECAP_BLEND_DESTINATION_COLOR = 4,
    SCENECAP_BLEND_ONE_MINUS_DESTINATION_COLOR = 5,
    SCENECAP_BLEND_SOURCE_ALPHA = 6,
    SCENECAP_BLEND_ONE_MINUS_SOURCE_ALPHA = 7,
    SCENECAP_BLEND_DESTINATION_ALPHA = 8,
    SCENECAP_BLEND_ONE_MINUS_DESTINATION_ALPHA = 9,
    SCENECAP_BLEND_SOURCE_ALPHA_SATURATED = 10,
    SCENECAP_BLEND_BLEND_COLOR = 11,
    SCENECAP_BLEND_ONE_MINUS_BLEND_COLOR = 12,
    SCENECAP_BLEND_BLEND_ALPHA = 13,
    SCENECAP_BLEND_ONE_MINUS_BLEND_ALPHA = 14
} scenecap_blend_factor;

typedef enum {
    SCENECAP_BLEND_OP_ADD = 0,
    SCENECAP_BLEND_OP_SUBTRACT = 1,
    SCENECAP_BLEND_OP_REVERSE_SUBTRACT = 2,
    SCENECAP_BLEND_OP_MIN = 3,
    SCENECAP_BLEND_OP_MAX = 4
} scenecap_blend_operation;

typedef enum {
    SCENECAP_COMPARE_NEVER = 0,
    SCENECAP_COMPARE_LESS = 1,
    SCENECAP_COMPARE_EQUAL = 2,
    SCENECAP_COMPARE_LESS_EQUAL = 3,
    SCENECAP_COMPARE_GREATER = 4,
    SCENECAP_COMPARE_NOT_EQUAL = 5,
    SCENECAP_COMPARE_GREATER_EQUAL = 6,
    SCENECAP_COMPARE_ALWAYS = 7
} scenecap_compare;

typedef enum {
    SCENECAP_CULL_NONE = 0,
    SCENECAP_CULL_FRONT = 1,
    SCENECAP_CULL_BACK = 2
} scenecap_cull_mode;

typedef enum {
    SCENECAP_FILL_FILL = 0,
    SCENECAP_FILL_LINES = 1,
    SCENECAP_FILL_POINTS = 2
} scenecap_fill_mode;

typedef enum {
    SCENECAP_FRONT_CW = 0,
    SCENECAP_FRONT_CCW = 1
} scenecap_front_face;

/* A render-pass attachment. slot 0..7 are colour attachments, 8 is depth, 9 is stencil. */
typedef struct {
    uint32_t slot;
    uint32_t has_texture;
    uint64_t texture; /* host texture serial */
    uint32_t level;
    uint32_t layer;
    uint32_t load;  /* scenecap_load_action */
    uint32_t store; /* scenecap_store_action */
    float clear[4];
    uint32_t clear_count;
} scenecap_attachment;

typedef struct {
    uint32_t kind; /* scenecap_pass_kind */
    uint32_t width;
    uint32_t height;
    const char *pixel_format;
    uint64_t serial; /* optional, recorded for reference */
    const scenecap_attachment *attachments;
    uint32_t attachment_count;
    const char *label;
} scenecap_pass;

/* One argument-table entry. TEXTURE/STORAGE_IMAGE refer to a texture serial, buffers to a guest address. */
typedef struct {
    uint32_t kind; /* scenecap_binding_kind */
    uint32_t stage;
    uint32_t index;
    uint64_t resource;
    uint64_t offset;
    uint64_t size;
    const void *data; /* optional CPU bytes for buffer kinds (Metal-only host lists / POSCAP) */
} scenecap_binding;

typedef struct {
    uint8_t enabled;
    uint8_t attachment_count;
    struct {
        uint8_t enabled;
        uint8_t rgb_operation;
        uint8_t alpha_operation;
        uint8_t rgb_source;
        uint8_t rgb_destination;
        uint8_t alpha_source;
        uint8_t alpha_destination;
        uint8_t write_mask;
    } attachments[8];
} scenecap_blend;

typedef struct {
    uint8_t depth_test;
    uint8_t depth_write;
    uint8_t depth_compare;
    uint8_t stencil_enabled;
    uint8_t stencil_front_compare;
    uint8_t stencil_back_compare;
    uint32_t stencil_read_mask;
    uint32_t stencil_write_mask;
} scenecap_depth_stencil;

typedef struct {
    uint8_t cull;
    uint8_t fill;
    uint8_t frontface;
    float depth_bias;
    float depth_slope_scale;
    float depth_clamp;
} scenecap_raster;

typedef struct {
    scenecap_blend blend;
    scenecap_depth_stencil depth_stencil;
    scenecap_raster raster;
    float viewport[4];
    float scissor[4];
} scenecap_state;

/* Vertex input layout. MSL vertex functions declare [[attribute(n)]], and Metal refuses a render pipeline that
 * has attributes but no vertex descriptor, so a replayer needs the attribute table, not only the buffers. */
#define SCENECAP_MAX_VERTEX_ATTRIBUTES 31
#define SCENECAP_MAX_VERTEX_LAYOUTS 31

typedef struct {
    uint32_t location;
    uint32_t format;       /* MTLVertexFormat value, copied from the port's own mapping */
    uint32_t offset;
    uint32_t buffer_index;
} scenecap_vertex_attribute;

typedef struct {
    uint32_t buffer_index;
    uint32_t stride;
    uint32_t step_function; /* MTLVertexStepFunction value */
    uint32_t step_rate;
} scenecap_vertex_layout;

/* A pipeline is identified by its shader variants; the state below is the state of the first draw recorded
 * for it. Every draw also carries its own state snapshot, so per-draw differences survive. */
typedef struct {
    const char *vertex_shader;
    const char *fragment_shader;
    const char *kernel_shader;
    scenecap_state state;
    scenecap_vertex_attribute attributes[SCENECAP_MAX_VERTEX_ATTRIBUTES];
    uint32_t attribute_count;
    scenecap_vertex_layout layouts[SCENECAP_MAX_VERTEX_LAYOUTS];
    uint32_t layout_count;
} scenecap_pipeline;

typedef struct {
    const scenecap_pipeline *pipeline;
    uint32_t vertex_start;
    uint32_t vertex_count;
    uint32_t index_type; /* scenecap_index_type */
    uint32_t index_start; /* first index into the index buffer (not base_vertex) */
    uint32_t index_count;
    uint64_t index_buffer; /* guest address of the index data; 0 when the draw is non-indexed */
    uint64_t index_bytes;
    /* Optional CPU snapshot of index_buffer (host survivor list). When non-NULL, the recorder
     * copies these bytes instead of host.buffer_read — needed for Metal-only list buffers that
     * are not in the guest pool map. length is index_bytes. */
    const void *index_data;
    uint32_t base_vertex; /* added to every fetched index; 0 when unused */
    uint32_t instances;
    uint32_t primitive; /* scenecap_primitive */
    const scenecap_binding *bindings;
    uint32_t binding_count;
    const char *label;
} scenecap_draw_desc;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mip_levels;
    uint32_t array_layers;
    uint32_t sample_count;
    uint32_t texture_type; /* 0 1d, 1 2d, 2 3d, 3 cube, 4 2d_array, 5 cube_array, 6 2d_multisample */
    char pixel_format[32];
} scenecap_texture_info;

typedef struct {
    void *user;
    /* Fills *out for the texture serial; returns 1 on success. */
    int (*texture_info)(void *user, uint64_t serial, scenecap_texture_info *out);
    /* Copies the tightly packed bytes of one mip level / array layer into dst; returns the byte count or -1. */
    long (*texture_read)(void *user, uint64_t serial, uint32_t level, uint32_t layer, void *dst, size_t capacity);
    /* Copies guest memory into dst; returns the byte count (== length) or -1. */
    long (*buffer_read)(void *user, uint64_t address, uint64_t length, void *dst, size_t capacity);
    /* Points *text at the MSL / reflection JSON of the shader variant; the host keeps it alive until the next
     * call on the same key. Returns 1 on success, 0 when unavailable. */
    int (*shader_source)(void *user, const char *key, const char **text, size_t *length);
    int (*shader_reflection)(void *user, const char *key, const char **text, size_t *length);
    void (*log)(void *user, const char *message);
} scenecap_host;

typedef struct {
    /* capture metadata; every field is copied */
    const char *game;
    const char *title_id;
    const char *game_version;
    const char *git_commit;
    const char *port_version;
    const char *build_version;
    const char *gpu;
    const char *macos;
    const char *metal;
    const char *captured_at;

    uint64_t first_frame;    /* first frame of the window; scenecap_options_from_env() leaves it at 0 */
    uint32_t frame_count;    /* frames in the window, at least 1 */

    const char *output_path; /* package path; a single %llu or '#' is replaced by the first frame number */
    const char *generator;   /* defaults to "scenecap-capture/1.0" */

    uint64_t max_texture_bytes; /* per texture, default 64 MiB */
    uint64_t max_buffer_bytes;  /* per buffer, default 16 MiB */
    uint64_t max_total_bytes;   /* over all blobs, default 512 MiB */
    int compress;               /* 0 = store blobs verbatim (the default), 1 = let the writer compress */
} scenecap_options;

typedef struct scenecap_capture scenecap_capture;

/* Fills *options from the environment (SCENECAP, SCENECAP_PKG, SCENECAP_FRAMES, SCENECAP_MAX_BYTES).
 * Returns 1 when SCENECAP=1 asked for capture; the metadata strings are left for the caller. */
int scenecap_options_from_env(scenecap_options *options);

/* Creates a capture object; the host callbacks must stay valid until destroy. Returns NULL on failure and
 * stores a message in err (when err_size > 0). */
scenecap_capture *scenecap_create(const scenecap_host *host, const scenecap_options *options, char *err, size_t err_size);
void scenecap_destroy(scenecap_capture *capture);

/* Called once per game frame before the frame's commands are replayed. The window is [first_frame, first_frame
 * + frame_count). When frame reaches the end of the window the package is written and the capture goes idle. */
void scenecap_frame_begin(scenecap_capture *capture, uint64_t frame);

/* Recording hooks; all no-ops unless the capture is inside its window. */
void scenecap_pass_begin(scenecap_capture *capture, const scenecap_pass *pass);
void scenecap_pass_end(scenecap_capture *capture);
void scenecap_draw(scenecap_capture *capture, const scenecap_draw_desc *draw);
void scenecap_dispatch(scenecap_capture *capture, const scenecap_pipeline *pipeline, const scenecap_binding *bindings,
                       uint32_t binding_count, const uint32_t grid[3], const char *label);

/* Writes the package now (the window ends early); returns 1 on success. */
int scenecap_finish(scenecap_capture *capture, char *err, size_t err_size);
/* 1 while the window is open, 0 otherwise. */
int scenecap_active(const scenecap_capture *capture);
/* Number of draws recorded so far. */
uint64_t scenecap_draw_count(const scenecap_capture *capture);
/* The last error message, or "" when there was none. */
const char *scenecap_last_error(const scenecap_capture *capture);
/* Path of the last written package, or "" when nothing was written. */
const char *scenecap_package_path(const scenecap_capture *capture);

#ifdef __cplusplus
}
#endif

#endif /* SCENECAP_CAPTURE_H */

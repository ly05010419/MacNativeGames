/* scenecap capture module: structured scene recording into a .nvs package.
 *
 * This file is plain C11 and deliberately knows nothing about Metal or Objective-C: the replay layer hands over
 * numbers and bytes through scenecap_host, and this module turns them into the manifest described in
 * tools/scenecap/README.md plus one blob per referenced shader, texture and buffer.
 */
#include "scenecap_capture.h"

/* tools/scenecap keeps the container writer one directory over (format/); a synchronised game tree keeps both
 * files beside each other (tools/scenecap/sync.py copies the kernel into gfx/nvn/).  Resolve either layout. */
#if defined(__has_include)
#  if __has_include("nvs_format.h")
#    include "nvs_format.h"
#  else
#    include "../format/nvs_format.h"
#  endif
#else
#  include "../format/nvs_format.h"
#endif

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SC_MAX_ATTACH 10u
#define SC_MAX_STAGES 3u
#define SC_SHADER_LIMIT (8u * 1024u * 1024u)
#define SC_DEFAULT_TEXTURE_LIMIT (64ull * 1024ull * 1024ull)
#define SC_DEFAULT_BUFFER_LIMIT (16ull * 1024ull * 1024ull)
#define SC_DEFAULT_TOTAL_LIMIT (512ull * 1024ull * 1024ull)

/* ---------------------------------------------------------------------------------------------------------- *
 * Small helpers: growable text, JSON escaping, identifiers, names.
 * ---------------------------------------------------------------------------------------------------------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int oom;
} text_buf;

static void tb_init(text_buf *tb) {
    tb->data = NULL;
    tb->len = 0;
    tb->cap = 0;
    tb->oom = 0;
}

static void tb_free(text_buf *tb) {
    free(tb->data);
    tb_init(tb);
}

static int tb_reserve(text_buf *tb, size_t extra) {
    if (tb->oom) return 0;
    if (tb->len + extra + 1 <= tb->cap) return 1;
    size_t want = tb->cap ? tb->cap : 4096;
    while (want < tb->len + extra + 1) want *= 2;
    char *next = realloc(tb->data, want);
    if (!next) {
        tb->oom = 1;
        return 0;
    }
    tb->data = next;
    tb->cap = want;
    return 1;
}

static void tb_add(text_buf *tb, const char *text, size_t len) {
    if (!text || !len) return;
    if (!tb_reserve(tb, len)) return;
    memcpy(tb->data + tb->len, text, len);
    tb->len += len;
    tb->data[tb->len] = 0;
}

static void tb_puts(text_buf *tb, const char *text) {
    tb_add(tb, text, text ? strlen(text) : 0);
}

static void tb_addf(text_buf *tb, const char *fmt, ...) {
    char stack[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(stack, sizeof stack, fmt, args);
    va_end(args);
    if (n < 0) {
        tb->oom = 1;
        return;
    }
    if ((size_t)n < sizeof stack) {
        tb_add(tb, stack, (size_t)n);
        return;
    }
    char *heap = malloc((size_t)n + 1);
    if (!heap) {
        tb->oom = 1;
        return;
    }
    va_start(args, fmt);
    vsnprintf(heap, (size_t)n + 1, fmt, args);
    va_end(args);
    tb_add(tb, heap, (size_t)n);
    free(heap);
}

static void tb_json_string(text_buf *tb, const char *text) {
    tb_add(tb, "\"", 1);
    if (text) {
        for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
            switch (*p) {
            case '"': tb_add(tb, "\\\"", 2); break;
            case '\\': tb_add(tb, "\\\\", 2); break;
            case '\b': tb_add(tb, "\\b", 2); break;
            case '\f': tb_add(tb, "\\f", 2); break;
            case '\n': tb_add(tb, "\\n", 2); break;
            case '\r': tb_add(tb, "\\r", 2); break;
            case '\t': tb_add(tb, "\\t", 2); break;
            default:
                if (*p < 0x20) tb_addf(tb, "\\u%04x", *p);
                else tb_add(tb, (const char *)p, 1);
            }
        }
    }
    tb_add(tb, "\"", 1);
}

/* Chunk ids may only contain [A-Za-z0-9._:/-]; anything else becomes '_'. */
static void id_sanitize(char *out, size_t cap, const char *in) {
    size_t o = 0;
    for (const char *p = in ? in : ""; *p && o + 1 < cap; p++) {
        unsigned char ch = (unsigned char)*p;
        int ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' ||
                 ch == '_' || ch == ':' || ch == '/' || ch == '-';
        out[o++] = ok ? (char)ch : '_';
    }
    out[o] = 0;
    if (o == 0 && cap > 1) {
        out[0] = '_';
        out[1] = 0;
    }
}

static const char *blend_op_name(int op) {
    switch (op) {
    case SCENECAP_BLEND_OP_SUBTRACT: return "subtract";
    case SCENECAP_BLEND_OP_REVERSE_SUBTRACT: return "reverse_subtract";
    case SCENECAP_BLEND_OP_MIN: return "min";
    case SCENECAP_BLEND_OP_MAX: return "max";
    default: return "add";
    }
}

static const char *blend_factor_name(int factor) {
    switch (factor) {
    case SCENECAP_BLEND_ONE: return "one";
    case SCENECAP_BLEND_SOURCE_COLOR: return "source_color";
    case SCENECAP_BLEND_ONE_MINUS_SOURCE_COLOR: return "one_minus_source_color";
    case SCENECAP_BLEND_DESTINATION_COLOR: return "destination_color";
    case SCENECAP_BLEND_ONE_MINUS_DESTINATION_COLOR: return "one_minus_destination_color";
    case SCENECAP_BLEND_SOURCE_ALPHA: return "source_alpha";
    case SCENECAP_BLEND_ONE_MINUS_SOURCE_ALPHA: return "one_minus_source_alpha";
    case SCENECAP_BLEND_DESTINATION_ALPHA: return "destination_alpha";
    case SCENECAP_BLEND_ONE_MINUS_DESTINATION_ALPHA: return "one_minus_destination_alpha";
    case SCENECAP_BLEND_SOURCE_ALPHA_SATURATED: return "source_alpha_saturated";
    case SCENECAP_BLEND_BLEND_COLOR: return "blend_color";
    case SCENECAP_BLEND_ONE_MINUS_BLEND_COLOR: return "one_minus_blend_color";
    case SCENECAP_BLEND_BLEND_ALPHA: return "blend_alpha";
    case SCENECAP_BLEND_ONE_MINUS_BLEND_ALPHA: return "one_minus_blend_alpha";
    default: return "zero";
    }
}

static const char *compare_name(int compare) {
    switch (compare) {
    case SCENECAP_COMPARE_LESS: return "less";
    case SCENECAP_COMPARE_EQUAL: return "equal";
    case SCENECAP_COMPARE_LESS_EQUAL: return "less_equal";
    case SCENECAP_COMPARE_GREATER: return "greater";
    case SCENECAP_COMPARE_NOT_EQUAL: return "not_equal";
    case SCENECAP_COMPARE_GREATER_EQUAL: return "greater_equal";
    case SCENECAP_COMPARE_ALWAYS: return "always";
    default: return "never";
    }
}

static const char *cull_name(int cull) {
    switch (cull) {
    case SCENECAP_CULL_FRONT: return "front";
    case SCENECAP_CULL_BACK: return "back";
    default: return "none";
    }
}

static const char *fill_name(int fill) {
    switch (fill) {
    case SCENECAP_FILL_LINES: return "lines";
    case SCENECAP_FILL_POINTS: return "points";
    default: return "fill";
    }
}

static const char *front_name(int front) { return front == SCENECAP_FRONT_CCW ? "ccw" : "cw"; }

static const char *prim_name(int prim) {
    switch (prim) {
    case SCENECAP_PRIM_POINT: return "point";
    case SCENECAP_PRIM_LINE: return "line";
    case SCENECAP_PRIM_LINE_STRIP: return "line_strip";
    case SCENECAP_PRIM_TRIANGLE_STRIP: return "triangle_strip";
    default: return "triangle";
    }
}

static const char *index_name(int type) {
    switch (type) {
    case SCENECAP_INDEX_UINT16: return "uint16";
    case SCENECAP_INDEX_UINT32: return "uint32";
    case SCENECAP_INDEX_UINT8: return "uint16"; /* NVN 8-bit indices are widened to uint16 before the draw */
    default: return "uint16";
    }
}

static const char *stage_name(int stage) {
    switch (stage) {
    case SCENECAP_STAGE_FRAGMENT: return "fragment";
    case SCENECAP_STAGE_KERNEL: return "kernel";
    case SCENECAP_STAGE_OBJECT: return "object";
    case SCENECAP_STAGE_MESH: return "mesh";
    case SCENECAP_STAGE_TILE: return "tile";
    default: return "vertex";
    }
}

static const char *texture_type_name(int type) {
    switch (type) {
    case 0: return "1d";
    case 2: return "3d";
    case 3: return "cube";
    case 4: return "2d_array";
    case 5: return "cube_array";
    case 6: return "2d_multisample";
    default: return "2d";
    }
}

static const char *pass_kind_name(int kind) {
    switch (kind) {
    case SCENECAP_PASS_COMPUTE: return "compute";
    case SCENECAP_PASS_BLIT: return "blit";
    default: return "render";
    }
}

static const char *load_name(int load) {
    switch (load) {
    case SCENECAP_LOAD_CLEAR: return "clear";
    case SCENECAP_LOAD_DONTCARE: return "dontcare";
    default: return "load";
    }
}

static const char *store_name(int store) {
    return store == SCENECAP_STORE_DONTCARE ? "dontcare" : "store";
}

/* ---------------------------------------------------------------------------------------------------------- *
 * Records.
 * ---------------------------------------------------------------------------------------------------------- */

typedef struct {
    char id[96];
    int is_texture;
    int skipped;
    int used;
    uint64_t serial;      /* textures */
    uint64_t address;     /* buffers */
    uint64_t size;
    uint32_t level, layer; /* the referenced mip level / array layer */
    scenecap_texture_info info;
    uint8_t *data;        /* owned copy of the bytes */
    size_t data_size;
    uint32_t usage;       /* SC_USE_* bits collected from every reference */
} res_rec;

#define SC_USE_RENDER_TARGET 1u
#define SC_USE_DEPTH_STENCIL 2u
#define SC_USE_SAMPLE 4u
#define SC_USE_VERTEX 8u
#define SC_USE_INDEX 16u
#define SC_USE_UNIFORM 32u
#define SC_USE_STORAGE 64u

static void mark_usage(scenecap_capture *c, int32_t res, uint32_t bits);


typedef struct {
    char key[96];
    int stage; /* scenecap_stage */
    char msl_id[160];
    char refl_id[160];
    uint8_t *msl;
    size_t msl_size;
    uint8_t *refl;
    size_t refl_size;
    int have_msl;
    int have_refl;
} stage_rec;

typedef struct {
    char key[200];
    char id[32];
    stage_rec stages[SC_MAX_STAGES];
    uint32_t stage_count;
    scenecap_state state;
    int have_state;
    scenecap_vertex_attribute attributes[SCENECAP_MAX_VERTEX_ATTRIBUTES];
    uint32_t attribute_count;
    scenecap_vertex_layout layouts[SCENECAP_MAX_VERTEX_LAYOUTS];
    uint32_t layout_count;
} pipe_rec;

typedef struct {
    int32_t res;
    uint32_t slot, load, store, clear_count;
    float clear[4];
} att_rec;

typedef struct {
    char id[32];
    char label[64];
    uint32_t kind, width, height;
    char pixel_format[32];
    uint64_t frame;
    att_rec attachments[SC_MAX_ATTACH];
    uint32_t attachment_count;
    uint32_t *draws;
    uint32_t draw_count, draw_cap;
    uint32_t *pipes;
    uint32_t pipe_count, pipe_cap;
    int kept;
} pass_rec;

typedef struct {
    char id[32];
    char label[64];
    uint32_t pass;
    int32_t pipe;
    uint32_t vertex_start, vertex_count;
    uint32_t index_type, index_start, index_count;
    uint32_t base_vertex;
    int32_t index_res;
    uint32_t instances, primitive;
    uint32_t bind_first, bind_count;
    scenecap_state state;
    int kept;
} draw_rec;

typedef struct {
    char id[32];
    char label[64];
    uint32_t pass;
    int32_t pipe;
    uint32_t grid[3];
    uint32_t bind_first, bind_count;
    int kept;
} dispatch_rec;

typedef struct {
    uint32_t kind, stage, index;
    int32_t res;
    uint64_t offset, size;
} bind_rec;

struct scenecap_capture {
    scenecap_host host;
    scenecap_options opt;
    char err[512];
    char pkg_path[1024];
    int wrote;

    uint64_t first_frame;
    uint32_t frame_count;
    uint64_t cur_frame;
    int state; /* 0 idle, 1 recording, 2 finished, 3 failed */

    res_rec *res;
    size_t res_count, res_cap;
    pipe_rec *pipes;
    size_t pipe_count, pipe_cap;
    pass_rec *passes;
    size_t pass_count, pass_cap;
    draw_rec *draws;
    size_t draw_count, draw_cap;
    dispatch_rec *dispatches;
    size_t dispatch_count, dispatch_cap;
    bind_rec *binds;
    size_t bind_count, bind_cap;

    int32_t open_pass;
    uint64_t total_bytes;
    uint64_t dropped_draws;
    uint64_t dropped_resources;

    /* Blob ids already written: one shader can serve several pipelines and must be stored only once. */
    char **blob_ids;
    size_t blob_id_count, blob_id_cap;
};

static void mark_usage(scenecap_capture *c, int32_t res, uint32_t bits) {
    if (res >= 0 && (size_t)res < c->res_count) c->res[res].usage |= bits;
}

/* ---------------------------------------------------------------------------------------------------------- *
 * Errors and logging.
 * ---------------------------------------------------------------------------------------------------------- */

static void sc_error(scenecap_capture *c, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, args);
    va_end(args);
}

static void sc_log(scenecap_capture *c, const char *fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    if (c->host.log) c->host.log(c->host.user, line);
}

/* ---------------------------------------------------------------------------------------------------------- *
 * Dynamic arrays.
 * ---------------------------------------------------------------------------------------------------------- */

static int sc_grow(void **array, size_t *cap, size_t count, size_t elem) {
    if (count < *cap) return 1;
    size_t next = *cap ? *cap * 2 : 32;
    void *grown = realloc(*array, next * elem);
    if (!grown) return 0;
    *array = grown;
    *cap = next;
    return 1;
}

static int pass_push_draw(pass_rec *pass, uint32_t index) {
    if (pass->draw_count == pass->draw_cap) {
        size_t next = pass->draw_cap ? pass->draw_cap * 2 : 16;
        uint32_t *grown = realloc(pass->draws, next * sizeof *grown);
        if (!grown) return 0;
        pass->draws = grown;
        pass->draw_cap = (uint32_t)next;
    }
    pass->draws[pass->draw_count++] = index;
    return 1;
}

static int pass_push_pipe(pass_rec *pass, uint32_t index) {
    for (uint32_t i = 0; i < pass->pipe_count; i++)
        if (pass->pipes[i] == index) return 1;
    if (pass->pipe_count == pass->pipe_cap) {
        size_t next = pass->pipe_cap ? pass->pipe_cap * 2 : 8;
        uint32_t *grown = realloc(pass->pipes, next * sizeof *grown);
        if (!grown) return 0;
        pass->pipes = grown;
        pass->pipe_cap = (uint32_t)next;
    }
    pass->pipes[pass->pipe_count++] = index;
    return 1;
}

/* ---------------------------------------------------------------------------------------------------------- *
 * Interning.
 * ---------------------------------------------------------------------------------------------------------- */

static int32_t intern_texture(scenecap_capture *c, uint64_t serial, uint32_t level, uint32_t layer) {
    for (size_t i = 0; i < c->res_count; i++) {
        res_rec *r = &c->res[i];
        if (r->is_texture && r->serial == serial && r->level == level && r->layer == layer) return (int32_t)i;
    }
    if (!sc_grow((void **)&c->res, &c->res_cap, c->res_count, sizeof *c->res)) return -1;
    res_rec *r = &c->res[c->res_count];
    memset(r, 0, sizeof *r);
    r->is_texture = 1;
    r->serial = serial;
    r->level = level;
    r->layer = layer;
    snprintf(r->id, sizeof r->id, "tex/%" PRIu64 "/%u/%u", serial, level, layer);
    if (c->host.texture_info) {
        if (!c->host.texture_info(c->host.user, serial, &r->info)) {
            r->info.width = 1;
            r->info.height = 1;
            r->info.depth = 1;
            r->info.mip_levels = 1;
            r->info.array_layers = 1;
            r->info.sample_count = 1;
            r->info.texture_type = 1;
            snprintf(r->info.pixel_format, sizeof r->info.pixel_format, "unknown");
            r->skipped = 1;
            c->dropped_resources++;
            sc_log(c, "scenecap: texture serial %" PRIu64 " has no metadata; dropping it", serial);
        }
    } else {
        r->skipped = 1;
        c->dropped_resources++;
    }
    c->res_count++;
    return (int32_t)(c->res_count - 1);
}

static int32_t intern_buffer_ex(scenecap_capture *c, uint64_t address, uint64_t size, const void *cpu_bytes) {
    if (size == 0) return -1;
    for (size_t i = 0; i < c->res_count; i++) {
        res_rec *r = &c->res[i];
        if (!r->is_texture && r->address == address && r->size == size) return (int32_t)i;
    }
    if (!sc_grow((void **)&c->res, &c->res_cap, c->res_count, sizeof *c->res)) return -1;
    res_rec *r = &c->res[c->res_count];
    memset(r, 0, sizeof *r);
    r->address = address;
    r->size = size;
    snprintf(r->id, sizeof r->id, "buf/%" PRIu64 "/%" PRIu64, address, size);
    uint64_t limit = c->opt.max_buffer_bytes ? c->opt.max_buffer_bytes : SC_DEFAULT_BUFFER_LIMIT;
    uint64_t total = c->opt.max_total_bytes ? c->opt.max_total_bytes : SC_DEFAULT_TOTAL_LIMIT;
    if (size > limit || c->total_bytes + size > total || (!cpu_bytes && !c->host.buffer_read)) {
        r->skipped = 1;
        c->dropped_resources++;
        sc_log(c, "scenecap: buffer %#" PRIx64 " (%" PRIu64 " bytes) over the capture budget; dropping it", address, size);
    } else {
        uint8_t *copy = malloc((size_t)size);
        if (!copy) {
            r->skipped = 1;
            c->dropped_resources++;
        } else if (cpu_bytes) {
            memcpy(copy, cpu_bytes, (size_t)size);
            r->data = copy;
            r->data_size = (size_t)size;
            c->total_bytes += size;
        } else {
            long got = c->host.buffer_read(c->host.user, address, size, copy, (size_t)size);
            if (got != (long)size) {
                free(copy);
                r->skipped = 1;
                c->dropped_resources++;
                sc_log(c, "scenecap: buffer %#" PRIx64 " is not readable; dropping it", address);
            } else {
                r->data = copy;
                r->data_size = (size_t)size;
                c->total_bytes += size;
            }
        }
    }
    c->res_count++;
    return (int32_t)(c->res_count - 1);
}

static int32_t intern_buffer(scenecap_capture *c, uint64_t address, uint64_t size) {
    return intern_buffer_ex(c, address, size, NULL);
}

/* FNV-1a over a shader key: the package chunk id is capped at 63 bytes, while a real key (a 64-hex
 * content digest plus a pipeline suffix) is longer. The full key still goes to the manifest. */
static uint64_t sc_hash64(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

/* Adds (or reuses) a shader stage record on a pipeline; returns its index. */
static uint32_t intern_stage(pipe_rec *pipe, const char *key, int stage) {
    char clean[96];
    id_sanitize(clean, sizeof clean, key);
    for (uint32_t i = 0; i < pipe->stage_count; i++)
        if (pipe->stages[i].stage == stage && strcmp(pipe->stages[i].key, clean) == 0) return i;
    if (pipe->stage_count >= SC_MAX_STAGES) return 0;
    stage_rec *rec = &pipe->stages[pipe->stage_count];
    memset(rec, 0, sizeof *rec);
    snprintf(rec->key, sizeof rec->key, "%s", clean);
    rec->stage = stage;
    char name[12];
    snprintf(name, sizeof name, "%s", stage_name(stage));
    const uint64_t tag = sc_hash64(clean);
    snprintf(rec->msl_id, sizeof rec->msl_id, "msl/%016llx/%s", (unsigned long long)tag, name);
    snprintf(rec->refl_id, sizeof rec->refl_id, "refl/%016llx/%s", (unsigned long long)tag, name);
    return pipe->stage_count++;
}

static int32_t intern_pipeline(scenecap_capture *c, const scenecap_pipeline *p, uint32_t primitive) {
    if (!p) return -1;
    char raw[200];
    /* The same shader can be used with different stream strides, and the vertex descriptor depends on both, so
     * the layout takes part in the identity of a pipeline. */
    uint64_t layout_hash = 1469598103934665603ull;
    for (uint32_t a = 0; a < p->attribute_count && a < SCENECAP_MAX_VERTEX_ATTRIBUTES; a++) {
        const uint32_t words[4] = {p->attributes[a].location, p->attributes[a].format, p->attributes[a].offset,
                                   p->attributes[a].buffer_index};
        for (int w = 0; w < 4; w++) {
            layout_hash ^= words[w];
            layout_hash *= 1099511628211ull;
        }
    }
    for (uint32_t l = 0; l < p->layout_count && l < SCENECAP_MAX_VERTEX_LAYOUTS; l++) {
        const uint32_t words[4] = {p->layouts[l].buffer_index, p->layouts[l].stride, p->layouts[l].step_function,
                                   p->layouts[l].step_rate};
        for (int w = 0; w < 4; w++) {
            layout_hash ^= words[w];
            layout_hash *= 1099511628211ull;
        }
    }
    if (p->kernel_shader && p->kernel_shader[0])
        snprintf(raw, sizeof raw, "k|%s|%016llx", p->kernel_shader, (unsigned long long)layout_hash);
    else
        snprintf(raw, sizeof raw, "r|%s|%s|%016llx", p->vertex_shader ? p->vertex_shader : "",
                 p->fragment_shader ? p->fragment_shader : "", (unsigned long long)layout_hash);
    for (size_t i = 0; i < c->pipe_count; i++)
        if (strcmp(c->pipes[i].key, raw) == 0) return (int32_t)i;
    if (!sc_grow((void **)&c->pipes, &c->pipe_cap, c->pipe_count, sizeof *c->pipes)) return -1;
    pipe_rec *rec = &c->pipes[c->pipe_count];
    memset(rec, 0, sizeof *rec);
    snprintf(rec->key, sizeof rec->key, "%s", raw);
    snprintf(rec->id, sizeof rec->id, "pipe/%zu", c->pipe_count);
    if (p->kernel_shader && p->kernel_shader[0]) {
        intern_stage(rec, p->kernel_shader, SCENECAP_STAGE_KERNEL);
    } else {
        intern_stage(rec, p->vertex_shader ? p->vertex_shader : "vs-none", SCENECAP_STAGE_VERTEX);
        intern_stage(rec, p->fragment_shader ? p->fragment_shader : "fs-none", SCENECAP_STAGE_FRAGMENT);
    }
    rec->state = p->state;
    rec->attribute_count = p->attribute_count < SCENECAP_MAX_VERTEX_ATTRIBUTES ? p->attribute_count
                                                                              : SCENECAP_MAX_VERTEX_ATTRIBUTES;
    for (uint32_t a = 0; a < rec->attribute_count; a++) rec->attributes[a] = p->attributes[a];
    rec->layout_count = p->layout_count < SCENECAP_MAX_VERTEX_LAYOUTS ? p->layout_count : SCENECAP_MAX_VERTEX_LAYOUTS;
    for (uint32_t l = 0; l < rec->layout_count; l++) rec->layouts[l] = p->layouts[l];
    (void)primitive;
    rec->have_state = 1;
    c->pipe_count++;
    return (int32_t)(c->pipe_count - 1);
}

/* ---------------------------------------------------------------------------------------------------------- *
 * Capture hooks.
 * ---------------------------------------------------------------------------------------------------------- */

static int sc_in_window(const scenecap_capture *c) {
    return c && c->state == 1;
}

void scenecap_frame_begin(scenecap_capture *c, uint64_t frame) {
    if (!c) return;
    c->cur_frame = frame;
    if (c->state == 0) {
        if (frame >= c->first_frame) {
            /* A request can arrive long after boot (the "scenecap <path>" input command), when the frame
             * counter is already past the configured first frame: align the window to now instead of
             * waiting for a first frame that will never come again. */
            if (frame > c->first_frame) c->first_frame = frame;
            c->state = 1;
            c->open_pass = -1;
        }
        return;
    }
    if (c->state != 1) return;
    if (frame >= c->first_frame + c->frame_count) {
        char message[sizeof c->err];
        if (!scenecap_finish(c, message, sizeof message)) {
            /* Never fail silently: a capture that could not be written has to say why on the console. */
            fprintf(stderr, "[scenecap] capture failed: %s\n", message[0] ? message : c->err);
            return;
        }
    }
}

void scenecap_pass_begin(scenecap_capture *c, const scenecap_pass *pass) {
    if (!sc_in_window(c) || !pass) return;
    if (!sc_grow((void **)&c->passes, &c->pass_cap, c->pass_count, sizeof *c->passes)) {
        sc_error(c, "out of memory while recording pass %zu", c->pass_count);
        c->state = 3;
        return;
    }
    pass_rec *rec = &c->passes[c->pass_count];
    memset(rec, 0, sizeof *rec);
    snprintf(rec->id, sizeof rec->id, "pass/%zu", c->pass_count);
    rec->kind = pass->kind;
    rec->width = pass->width ? pass->width : 1;
    rec->height = pass->height ? pass->height : 1;
    snprintf(rec->pixel_format, sizeof rec->pixel_format, "%s", pass->pixel_format && pass->pixel_format[0]
                                                                     ? pass->pixel_format
                                                                     : "unknown");
    if (pass->label) snprintf(rec->label, sizeof rec->label, "%s", pass->label);
    rec->frame = c->cur_frame;
    for (uint32_t i = 0; i < pass->attachment_count && rec->attachment_count < SC_MAX_ATTACH; i++) {
        const scenecap_attachment *src = &pass->attachments[i];
        att_rec *dst = &rec->attachments[rec->attachment_count++];
        dst->slot = src->slot;
        dst->load = src->load;
        dst->store = src->store;
        dst->clear_count = src->clear_count > 4 ? 4 : src->clear_count;
        for (uint32_t k = 0; k < 4; k++) dst->clear[k] = k < src->clear_count ? src->clear[k] : 0.0f;
        dst->res = src->has_texture ? intern_texture(c, src->texture, src->level, src->layer) : -1;
        mark_usage(c, dst->res, src->slot == 8 || src->slot == 9 ? SC_USE_DEPTH_STENCIL : SC_USE_RENDER_TARGET);
    }
    c->open_pass = (int32_t)c->pass_count;
    c->passes[c->pass_count].kept = 1;
    c->pass_count++;
}

void scenecap_pass_end(scenecap_capture *c) {
    if (!c) return;
    c->open_pass = -1;
}

void scenecap_draw(scenecap_capture *c, const scenecap_draw_desc *draw) {
    if (!sc_in_window(c) || !draw || c->open_pass < 0) return;
    pass_rec *pass = &c->passes[c->open_pass];
    int32_t pipe = intern_pipeline(c, draw->pipeline, draw->primitive);
    if (pipe < 0) {
        c->dropped_draws++;
        return;
    }
    if (!sc_grow((void **)&c->draws, &c->draw_cap, c->draw_count, sizeof *c->draws)) {
        sc_error(c, "out of memory while recording draw %zu", c->draw_count);
        c->state = 3;
        return;
    }
    draw_rec *rec = &c->draws[c->draw_count];
    memset(rec, 0, sizeof *rec);
    snprintf(rec->id, sizeof rec->id, "draw/%zu", c->draw_count);
    if (draw->label) snprintf(rec->label, sizeof rec->label, "%s", draw->label);
    rec->pass = (uint32_t)c->open_pass;
    rec->pipe = pipe;
    rec->vertex_start = draw->vertex_start;
    rec->vertex_count = draw->vertex_count ? draw->vertex_count : 1;
    rec->index_type = draw->index_type;
    rec->index_start = draw->index_start;
    rec->index_count = draw->index_count;
    rec->base_vertex = draw->base_vertex;
    rec->index_res = draw->index_buffer && draw->index_bytes
                         ? intern_buffer_ex(c, draw->index_buffer, draw->index_bytes, draw->index_data)
                         : -1;
    mark_usage(c, rec->index_res, SC_USE_INDEX);
    rec->instances = draw->instances ? draw->instances : 1;
    rec->primitive = draw->primitive;
    rec->state = draw->pipeline->state;
    rec->bind_first = (uint32_t)c->bind_count;
    for (uint32_t i = 0; i < draw->binding_count; i++) {
        const scenecap_binding *b = &draw->bindings[i];
        int32_t res = -1;
        if (b->kind == SCENECAP_BIND_TEXTURE || b->kind == SCENECAP_BIND_STORAGE_IMAGE) {
            if (b->resource) res = intern_texture(c, b->resource, 0, 0);
        } else if (b->kind != SCENECAP_BIND_SAMPLER && b->size) {
            res = intern_buffer_ex(c, b->resource, b->size, b->data);
        }
        if (!sc_grow((void **)&c->binds, &c->bind_cap, c->bind_count, sizeof *c->binds)) {
            rec->bind_count = 0;
            break;
        }
        bind_rec *brec = &c->binds[c->bind_count++];
        brec->kind = b->kind;
        brec->stage = b->stage;
        brec->index = b->index;
        brec->offset = b->offset;
        brec->size = b->size;
        brec->res = res;
        mark_usage(c, res, b->kind == SCENECAP_BIND_VERTEX_BUFFER ? SC_USE_VERTEX
                             : (b->kind == SCENECAP_BIND_UNIFORM_BUFFER || b->kind == SCENECAP_BIND_FRAGMENT_BUFFER
                                    ? SC_USE_UNIFORM
                                    : (b->kind == SCENECAP_BIND_TEXTURE ? SC_USE_SAMPLE : SC_USE_STORAGE)));
        rec->bind_count++;
    }
    rec->kept = 1;
    c->draws[c->draw_count].kept = pass_push_draw(pass, (uint32_t)c->draw_count) ? 1 : 0;
    pass_push_pipe(pass, (uint32_t)pipe);
    c->draw_count++;
}

void scenecap_dispatch(scenecap_capture *c, const scenecap_pipeline *pipeline, const scenecap_binding *bindings,
                       uint32_t binding_count, const uint32_t grid[3], const char *label) {
    if (!sc_in_window(c) || !pipeline) return;
    int32_t pipe = intern_pipeline(c, pipeline, 0);
    if (pipe < 0) return;
    /* A dispatch always runs outside a render encoder in this replay layer, so it gets its own compute pass. */
    scenecap_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.kind = SCENECAP_PASS_COMPUTE;
    pass.width = grid && grid[0] ? grid[0] : 1;
    pass.height = grid && grid[1] ? grid[1] : 1;
    pass.pixel_format = "none";
    pass.label = label;
    scenecap_pass_begin(c, &pass);
    if (!sc_in_window(c) || c->open_pass < 0) return;
    if (!sc_grow((void **)&c->dispatches, &c->dispatch_cap, c->dispatch_count, sizeof *c->dispatches)) {
        sc_error(c, "out of memory while recording dispatch %zu", c->dispatch_count);
        c->state = 3;
        return;
    }
    dispatch_rec *rec = &c->dispatches[c->dispatch_count];
    memset(rec, 0, sizeof *rec);
    snprintf(rec->id, sizeof rec->id, "dispatch/%zu", c->dispatch_count);
    if (label) snprintf(rec->label, sizeof rec->label, "%s", label);
    rec->pass = (uint32_t)c->open_pass;
    rec->pipe = pipe;
    rec->grid[0] = grid ? grid[0] : 1;
    rec->grid[1] = grid ? grid[1] : 1;
    rec->grid[2] = grid ? grid[2] : 1;
    rec->bind_first = (uint32_t)c->bind_count;
    for (uint32_t i = 0; i < binding_count; i++) {
        const scenecap_binding *b = &bindings[i];
        int32_t res = -1;
        if (b->kind == SCENECAP_BIND_TEXTURE || b->kind == SCENECAP_BIND_STORAGE_IMAGE) {
            if (b->resource) res = intern_texture(c, b->resource, 0, 0);
        } else if (b->kind != SCENECAP_BIND_SAMPLER && b->size) {
            res = intern_buffer_ex(c, b->resource, b->size, b->data);
        }
        if (!sc_grow((void **)&c->binds, &c->bind_cap, c->bind_count, sizeof *c->binds)) break;
        bind_rec *brec = &c->binds[c->bind_count++];
        brec->kind = b->kind;
        brec->stage = b->stage;
        brec->index = b->index;
        brec->offset = b->offset;
        brec->size = b->size;
        brec->res = res;
        mark_usage(c, res, b->kind == SCENECAP_BIND_VERTEX_BUFFER ? SC_USE_VERTEX
                             : (b->kind == SCENECAP_BIND_UNIFORM_BUFFER || b->kind == SCENECAP_BIND_FRAGMENT_BUFFER
                                    ? SC_USE_UNIFORM
                                    : (b->kind == SCENECAP_BIND_TEXTURE ? SC_USE_SAMPLE : SC_USE_STORAGE)));
        rec->bind_count++;
    }
    rec->kept = 1;
    pass_push_pipe(&c->passes[c->open_pass], (uint32_t)pipe);
    scenecap_pass_end(c);
    c->dispatch_count++;
}

int scenecap_active(const scenecap_capture *c) { return c && c->state == 1; }
uint64_t scenecap_draw_count(const scenecap_capture *c) { return c ? c->draw_count : 0; }
const char *scenecap_last_error(const scenecap_capture *c) { return c ? c->err : ""; }
const char *scenecap_package_path(const scenecap_capture *c) { return c ? c->pkg_path : ""; }

/* ---------------------------------------------------------------------------------------------------------- *
 * Manifest assembly.
 * ---------------------------------------------------------------------------------------------------------- */

static void emit_state(text_buf *tb, const scenecap_state *s, int primitive) {
    tb_puts(tb, "\"state\":{\"blend\":{\"enabled\":");
    tb_puts(tb, s->blend.enabled ? "true" : "false");
    tb_puts(tb, ",\"attachments\":[");
    for (uint32_t i = 0; i < s->blend.attachment_count && i < 8; i++) {
        if (i) tb_puts(tb, ",");
        tb_addf(tb,
                "{\"index\":%u,\"enabled\":%s,\"rgb_operation\":\"%s\",\"alpha_operation\":\"%s\","
                "\"rgb_source\":\"%s\",\"rgb_destination\":\"%s\",\"alpha_source\":\"%s\","
                "\"alpha_destination\":\"%s\",\"write_mask\":%u}",
                i, s->blend.attachments[i].enabled ? "true" : "false",
                blend_op_name(s->blend.attachments[i].rgb_operation),
                blend_op_name(s->blend.attachments[i].alpha_operation),
                blend_factor_name(s->blend.attachments[i].rgb_source),
                blend_factor_name(s->blend.attachments[i].rgb_destination),
                blend_factor_name(s->blend.attachments[i].alpha_source),
                blend_factor_name(s->blend.attachments[i].alpha_destination),
                s->blend.attachments[i].write_mask);
    }
    tb_puts(tb, "]},\"depth_stencil\":{\"depth_test\":");
    tb_puts(tb, s->depth_stencil.depth_test ? "true" : "false");
    tb_puts(tb, ",\"depth_write\":");
    tb_puts(tb, s->depth_stencil.depth_write ? "true" : "false");
    tb_addf(tb, ",\"depth_compare\":\"%s\",\"stencil\":{\"enabled\":%s,\"front\":{\"compare\":\"%s\"},"
                "\"back\":{\"compare\":\"%s\"},\"read_mask\":%u,\"write_mask\":%u}}",
            compare_name(s->depth_stencil.depth_compare),
            s->depth_stencil.stencil_enabled ? "true" : "false",
            compare_name(s->depth_stencil.stencil_front_compare),
            compare_name(s->depth_stencil.stencil_back_compare), s->depth_stencil.stencil_read_mask,
            s->depth_stencil.stencil_write_mask);
    tb_addf(tb, ",\"raster\":{\"cull\":\"%s\",\"fill\":\"%s\",\"depth_bias\":%g,\"depth_slope_scale\":%g,"
                "\"depth_clamp\":%g}",
            cull_name(s->raster.cull), fill_name(s->raster.fill), (double)s->raster.depth_bias,
            (double)s->raster.depth_slope_scale, (double)s->raster.depth_clamp);
    tb_addf(tb, ",\"viewport\":[%g,%g,%g,%g],\"scissor\":[%g,%g,%g,%g],\"frontface\":\"%s\","
                "\"primitive_type\":\"%s\"}",
            (double)s->viewport[0], (double)s->viewport[1], (double)s->viewport[2], (double)s->viewport[3],
            (double)s->scissor[0], (double)s->scissor[1], (double)s->scissor[2], (double)s->scissor[3],
            front_name(s->raster.frontface), prim_name(primitive));
}

static int pipe_usable(const scenecap_capture *c, int32_t pipe) {
    if (pipe < 0 || (size_t)pipe >= c->pipe_count) return 0;
    const pipe_rec *p = &c->pipes[pipe];
    for (uint32_t i = 0; i < p->stage_count; i++)
        if (!p->stages[i].have_msl) return 0;
    return p->stage_count > 0;
}

static int res_usable(const scenecap_capture *c, int32_t res) {
    if (res < 0 || (size_t)res >= c->res_count) return 0;
    const res_rec *r = &c->res[res];
    return !r->skipped && r->data && r->data_size;
}

static void emit_usage_list(text_buf *tb, const res_rec *r) {
    static const struct {
        uint32_t bit;
        const char *name;
    } names[] = {
        {SC_USE_RENDER_TARGET, "render_target"}, {SC_USE_DEPTH_STENCIL, "depth_stencil"},
        {SC_USE_SAMPLE, "sample"},               {SC_USE_VERTEX, "vertex"},
        {SC_USE_INDEX, "index"},                 {SC_USE_UNIFORM, "uniform"},
        {SC_USE_STORAGE, "storage"},
    };
    int wrote = 0;
    tb_puts(tb, "[");
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (!(r->usage & names[i].bit)) continue;
        tb_addf(tb, "%s\"%s\"", wrote ? "," : "", names[i].name);
        wrote = 1;
    }
    if (!wrote) tb_puts(tb, r->is_texture ? "\"sample\"" : "\"storage\"");
    tb_puts(tb, "]");
}


static void emit_binding_list(scenecap_capture *c, text_buf *tb, size_t first, uint32_t count, int kind_a, int kind_b,
                              const char *key, int *wrote) {
    int any = 0;
    for (uint32_t i = 0; i < count; i++) {
        const bind_rec *br = &c->binds[first + i];
        if ((int)br->kind != kind_a && (int)br->kind != kind_b) continue;
        if (br->kind != SCENECAP_BIND_SAMPLER) {
            if (br->res < 0 || !res_usable(c, br->res)) continue;
            c->res[br->res].used = 1;
        }
        if (!any) {
            tb_addf(tb, "%s\"%s\":[", *wrote ? "," : "", key);
            *wrote = 1;
            any = 1;
        } else {
            tb_puts(tb, ",");
        }
        if (br->kind == SCENECAP_BIND_SAMPLER) {
            tb_addf(tb, "{\"index\":%u,\"stage\":%u}", br->index, br->stage);
        } else if (br->kind == SCENECAP_BIND_TEXTURE || br->kind == SCENECAP_BIND_STORAGE_IMAGE) {
            tb_addf(tb, "{\"index\":%u,\"stage\":%u,\"texture\":", br->index, br->stage);
            tb_json_string(tb, c->res[br->res].id);
            tb_puts(tb, "}");
        } else {
            uint64_t length = br->offset + br->size <= c->res[br->res].data_size
                                  ? br->size
                                  : (br->offset < c->res[br->res].data_size ? c->res[br->res].data_size - br->offset : 0);
            tb_addf(tb, "{\"index\":%u,\"stage\":%u,\"buffer\":", br->index, br->stage);
            tb_json_string(tb, c->res[br->res].id);
            tb_addf(tb, ",\"offset\":%" PRIu64 ",\"length\":%" PRIu64 "}", br->offset, length);
        }
    }
    if (any) tb_puts(tb, "]");
}

static void emit_arg_table(scenecap_capture *c, text_buf *tb, size_t first, uint32_t count) {
    int wrote = 0;
    tb_puts(tb, ",\"argument_table\":{");
    emit_binding_list(c, tb, first, count, SCENECAP_BIND_VERTEX_BUFFER, SCENECAP_BIND_VERTEX_BUFFER,
                      "vertex_buffers", &wrote);
    emit_binding_list(c, tb, first, count, SCENECAP_BIND_TEXTURE, SCENECAP_BIND_STORAGE_IMAGE, "textures", &wrote);
    emit_binding_list(c, tb, first, count, SCENECAP_BIND_SAMPLER, SCENECAP_BIND_SAMPLER, "samplers", &wrote);
    emit_binding_list(c, tb, first, count, SCENECAP_BIND_UNIFORM_BUFFER, SCENECAP_BIND_UNIFORM_BUFFER,
                      "uniform_buffers", &wrote);
    emit_binding_list(c, tb, first, count, SCENECAP_BIND_STORAGE_BUFFER, SCENECAP_BIND_STORAGE_BUFFER,
                      "storage_buffers", &wrote);
    emit_binding_list(c, tb, first, count, SCENECAP_BIND_FRAGMENT_BUFFER, SCENECAP_BIND_FRAGMENT_BUFFER,
                      "fragment_buffers", &wrote);
    if (!wrote) tb_puts(tb, "\"vertex_buffers\":[]");
    tb_puts(tb, "}");
}

/* A pass is worth recording only if it can be replayed: a render pass needs one usable attachment, a compute pass
 * needs one dispatch whose pipeline was fully snapshotted. */
static int pass_content(scenecap_capture *c, pass_rec *p) {
    if (p->kind == SCENECAP_PASS_RENDER || p->kind == SCENECAP_PASS_BLIT) {
        for (uint32_t k = 0; k < p->attachment_count; k++) {
            int32_t res = p->attachments[k].res;
            if (res >= 0 && res_usable(c, res)) return 1;
        }
        return 0;
    }
    for (size_t i = 0; i < c->dispatch_count; i++) {
        if (c->dispatches[i].pass == (uint32_t)(p - c->passes) && pipe_usable(c, c->dispatches[i].pipe)) return 1;
    }
    return 0;
}

/* Emits the manifest and marks the resources that ended up referenced. Returns 0 on allocation failure. */
static int build_manifest(scenecap_capture *c, text_buf *tb) {
    const char *game = c->opt.game && c->opt.game[0] ? c->opt.game : "unknown";
    const char *title = c->opt.title_id && c->opt.title_id[0] ? c->opt.title_id : "unknown";
    const char *version = c->opt.game_version && c->opt.game_version[0] ? c->opt.game_version : "unknown";
    const char *commit = c->opt.git_commit && c->opt.git_commit[0] ? c->opt.git_commit : "unknown";
    const char *port = c->opt.port_version && c->opt.port_version[0] ? c->opt.port_version : "unknown";
    const char *build = c->opt.build_version && c->opt.build_version[0] ? c->opt.build_version : "unknown";
    const char *gpu = c->opt.gpu && c->opt.gpu[0] ? c->opt.gpu : "unknown";
    const char *macos = c->opt.macos && c->opt.macos[0] ? c->opt.macos : "unknown";
    const char *metal = c->opt.metal && c->opt.metal[0] ? c->opt.metal : "unknown";
    char stamp[32];
    if (c->opt.captured_at && c->opt.captured_at[0]) {
        snprintf(stamp, sizeof stamp, "%s", c->opt.captured_at);
    } else {
        time_t now = time(NULL);
        struct tm utc;
        struct tm *tmp = gmtime(&now);
        if (tmp) utc = *tmp; else memset(&utc, 0, sizeof utc);
        strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &utc);
    }

    /* Passes and draws are kept or dropped together: a draw without a usable pipeline cannot be replayed, and a
     * draw whose pass lists nothing would break the manifest's own consistency rules. */
    for (size_t i = 0; i < c->pass_count; i++) c->passes[i].kept = pass_content(c, &c->passes[i]);
    for (size_t i = 0; i < c->draw_count; i++) {
        c->draws[i].kept = pipe_usable(c, c->draws[i].pipe) && c->passes[c->draws[i].pass].kept;
        if (!c->draws[i].kept && pipe_usable(c, c->draws[i].pipe)) c->dropped_draws++;
    }
    for (size_t i = 0; i < c->dispatch_count; i++)
        c->dispatches[i].kept = pipe_usable(c, c->dispatches[i].pipe) && c->passes[c->dispatches[i].pass].kept;

    tb_puts(tb, "{\"capture\":{");
    tb_puts(tb, "\"game\":"); tb_json_string(tb, game);
    tb_puts(tb, ",\"title_id\":"); tb_json_string(tb, title);
    tb_puts(tb, ",\"game_version\":"); tb_json_string(tb, version);
    tb_puts(tb, ",\"git_commit\":"); tb_json_string(tb, commit);
    tb_puts(tb, ",\"port_version\":"); tb_json_string(tb, port);
    tb_puts(tb, ",\"build_version\":"); tb_json_string(tb, build);
    tb_puts(tb, ",\"gpu\":"); tb_json_string(tb, gpu);
    tb_puts(tb, ",\"macos\":"); tb_json_string(tb, macos);
    tb_puts(tb, ",\"metal\":"); tb_json_string(tb, metal);
    tb_puts(tb, ",\"captured_at\":"); tb_json_string(tb, stamp);
    tb_addf(tb, ",\"frame_first\":%" PRIu64 ",\"frame_count\":%u}", c->first_frame, c->frame_count);

    /* frames */
    tb_puts(tb, ",\"frames\":[");
    for (uint32_t f = 0; f < c->frame_count; f++) {
        uint64_t index = c->first_frame + f;
        if (f) tb_puts(tb, ",");
        tb_addf(tb, "{\"index\":%" PRIu64 ",\"passes\":[", index);
        int first = 1;
        for (size_t i = 0; i < c->pass_count; i++) {
            if (!c->passes[i].kept || c->passes[i].frame != index) continue;
            if (!first) tb_puts(tb, ",");
            first = 0;
            tb_json_string(tb, c->passes[i].id);
        }
        tb_puts(tb, "]}");
    }
    tb_puts(tb, "]");

    /* resources */
    tb_puts(tb, ",\"resources\":[");
    int first_res = 1;
    for (size_t i = 0; i < c->res_count; i++) {
        res_rec *r = &c->res[i];
        if (!res_usable(c, (int32_t)i)) continue;
        if (!first_res) tb_puts(tb, ",");
        first_res = 0;
        r->used = 1;
        tb_puts(tb, "{\"id\":"); tb_json_string(tb, r->id);
        tb_addf(tb, ",\"type\":\"%s\",\"blob\":", r->is_texture ? "texture" : "buffer");
        tb_json_string(tb, r->id);
        tb_addf(tb, ",\"bytes\":{\"offset\":0,\"size\":%zu},\"usage\":", r->data_size);
        emit_usage_list(tb, r);
        if (r->is_texture) {
            tb_puts(tb, ",\"pixel_format\":"); tb_json_string(tb, r->info.pixel_format);
            tb_addf(tb, ",\"width\":%u,\"height\":%u,\"depth\":%u,\"mip_levels\":%u,\"array_layers\":%u,"
                        "\"sample_count\":%u,\"texture_type\":\"%s\",\"mip_range\":{\"first\":%u,\"count\":1},"
                        "\"layer_range\":{\"first\":%u,\"count\":1}",
                    r->info.width, r->info.height, r->info.depth ? r->info.depth : 1,
                    r->info.mip_levels ? r->info.mip_levels : 1,
                    r->info.array_layers ? r->info.array_layers : 1,
                    r->info.sample_count ? r->info.sample_count : 1,
                    texture_type_name((int)r->info.texture_type), r->level, r->layer);
        } else {
            tb_addf(tb, ",\"bytes_stored\":%" PRIu64, r->size);
        }
        tb_puts(tb, "}");
    }
    tb_puts(tb, "]");

    /* pipelines */
    tb_puts(tb, ",\"pipelines\":[");
    int first_pipe = 1;
    for (size_t i = 0; i < c->pipe_count; i++) {
        pipe_rec *p = &c->pipes[i];
        if (!pipe_usable(c, (int32_t)i)) continue;
        if (!first_pipe) tb_puts(tb, ",");
        first_pipe = 0;
        tb_puts(tb, "{\"id\":"); tb_json_string(tb, p->id);
        tb_puts(tb, ",\"key\":"); tb_json_string(tb, p->key);
        tb_puts(tb, ",\"stages\":[");
        for (uint32_t s = 0; s < p->stage_count; s++) {
            if (s) tb_puts(tb, ",");
            tb_addf(tb, "{\"stage\":\"%s\",\"msl_blob\":", stage_name(p->stages[s].stage));
            tb_json_string(tb, p->stages[s].msl_id);
            if (p->stages[s].have_refl) {
                tb_puts(tb, ",\"reflection_blob\":");
                tb_json_string(tb, p->stages[s].refl_id);
            }
            tb_puts(tb, "}");
        }
        tb_puts(tb, "],");
        tb_puts(tb, "\"vertex_attributes\":[");
        for (uint32_t a = 0; a < p->attribute_count; a++) {
            if (a) tb_puts(tb, ",");
            tb_addf(tb, "{\"location\":%u,\"format\":%u,\"offset\":%u,\"buffer_index\":%u}",
                    p->attributes[a].location, p->attributes[a].format, p->attributes[a].offset,
                    p->attributes[a].buffer_index);
        }
        tb_puts(tb, "],\"vertex_layouts\":[");
        for (uint32_t l = 0; l < p->layout_count; l++) {
            if (l) tb_puts(tb, ",");
            tb_addf(tb, "{\"buffer_index\":%u,\"stride\":%u,\"step_function\":%u,\"step_rate\":%u}",
                    p->layouts[l].buffer_index, p->layouts[l].stride, p->layouts[l].step_function,
                    p->layouts[l].step_rate);
        }
        tb_puts(tb, "],");
        emit_state(tb, &p->state, SCENECAP_PRIM_TRIANGLE);
        tb_puts(tb, "}");
    }
    tb_puts(tb, "]");

    /* passes */
    tb_puts(tb, ",\"passes\":[");
    int first_pass = 1;
    for (size_t i = 0; i < c->pass_count; i++) {
        pass_rec *p = &c->passes[i];
        if (!p->kept) continue;
        if (!first_pass) tb_puts(tb, ",");
        first_pass = 0;
        tb_puts(tb, "{\"id\":"); tb_json_string(tb, p->id);
        tb_addf(tb, ",\"kind\":\"%s\",\"width\":%u,\"height\":%u,\"pixel_format\":", pass_kind_name((int)p->kind),
                p->width, p->height);
        tb_json_string(tb, p->pixel_format);
        tb_addf(tb, ",\"frame\":%" PRIu64, p->frame);
        if (p->label[0]) { tb_puts(tb, ",\"label\":"); tb_json_string(tb, p->label); }
        tb_puts(tb, ",\"attachments\":[");
        int first_att = 1;
        for (uint32_t k = 0; k < p->attachment_count; k++) {
            att_rec *a = &p->attachments[k];
            if (a->res < 0 || !res_usable(c, a->res)) continue;
            if (!first_att) tb_puts(tb, ",");
            first_att = 0;
            c->res[a->res].used = 1;
            tb_addf(tb, "{\"slot\":%u,\"texture\":", a->slot);
            tb_json_string(tb, c->res[a->res].id);
            tb_addf(tb, ",\"level\":%u,\"layer\":%u,\"load\":\"%s\",\"store\":\"%s\"", c->res[a->res].level,
                    c->res[a->res].layer, load_name((int)a->load), store_name((int)a->store));
            if (a->load == SCENECAP_LOAD_CLEAR && a->clear_count) {
                tb_puts(tb, ",\"clear\":[");
                for (uint32_t q = 0; q < a->clear_count; q++) {
                    if (q) tb_puts(tb, ",");
                    tb_addf(tb, "%g", (double)a->clear[q]);
                }
                tb_puts(tb, "]");
            }
            tb_puts(tb, "}");
        }
        tb_puts(tb, "],\"pipelines\":[");
        int first = 1;
        for (uint32_t k = 0; k < p->pipe_count; k++) {
            if (!pipe_usable(c, (int32_t)p->pipes[k])) continue;
            if (!first) tb_puts(tb, ",");
            first = 0;
            tb_json_string(tb, c->pipes[p->pipes[k]].id);
        }
        tb_puts(tb, "],\"draws\":[");
        first = 1;
        for (uint32_t k = 0; k < p->draw_count; k++) {
            uint32_t d = p->draws[k];
            if (!c->draws[d].kept) continue;
            if (!first) tb_puts(tb, ",");
            first = 0;
            tb_json_string(tb, c->draws[d].id);
        }
        tb_puts(tb, "]}");
    }
    tb_puts(tb, "]");

    /* draws */
    tb_puts(tb, ",\"draws\":[");
    int first_draw = 1;
    for (size_t i = 0; i < c->draw_count; i++) {
        draw_rec *d = &c->draws[i];
        if (!d->kept) continue;
        if (!first_draw) tb_puts(tb, ",");
        first_draw = 0;
        tb_puts(tb, "{\"id\":"); tb_json_string(tb, d->id);
        tb_puts(tb, ",\"pass\":"); tb_json_string(tb, c->passes[d->pass].id);
        tb_puts(tb, ",\"pipeline\":"); tb_json_string(tb, c->pipes[d->pipe].id);
        if (d->label[0]) { tb_puts(tb, ",\"label\":"); tb_json_string(tb, d->label); }
        tb_addf(tb, ",\"vertex\":{\"start\":%u,\"count\":%u}", d->vertex_start, d->vertex_count);
        if (d->index_type) {
            tb_addf(tb, ",\"index\":{\"type\":\"%s\",\"start\":%u,\"count\":%u",
                    index_name((int)d->index_type), d->index_start, d->index_count ? d->index_count : d->vertex_count);
            if (d->index_res >= 0) { tb_puts(tb, ",\"buffer\":"); tb_json_string(tb, c->res[d->index_res].id); }
            if (d->base_vertex) tb_addf(tb, ",\"base_vertex\":%u", d->base_vertex);
            tb_puts(tb, "}");
        }
        tb_addf(tb, ",\"instances\":%u,\"primitive_type\":\"%s\"", d->instances, prim_name((int)d->primitive));
        emit_arg_table(c, tb, d->bind_first, d->bind_count);
        tb_puts(tb, ",");
        emit_state(tb, &d->state, (int)d->primitive);
        tb_puts(tb, "}");
    }
    tb_puts(tb, "]");

    /* dispatches */
    tb_puts(tb, ",\"dispatches\":[");
    int first_disp = 1;
    for (size_t i = 0; i < c->dispatch_count; i++) {
        dispatch_rec *d = &c->dispatches[i];
        if (!d->kept) continue;
        if (!first_disp) tb_puts(tb, ",");
        first_disp = 0;
        tb_puts(tb, "{\"id\":"); tb_json_string(tb, d->id);
        tb_puts(tb, ",\"pass\":"); tb_json_string(tb, c->passes[d->pass].id);
        tb_puts(tb, ",\"pipeline\":"); tb_json_string(tb, c->pipes[d->pipe].id);
        tb_addf(tb, ",\"grid\":[%u,%u,%u]", d->grid[0], d->grid[1], d->grid[2]);
        emit_arg_table(c, tb, d->bind_first, d->bind_count);
        tb_puts(tb, "}");
    }
    tb_puts(tb, "]");

    if (c->dropped_draws || c->dropped_resources) {
        tb_addf(tb, ",\"warnings\":{\"notes\":[\"capture dropped records that could not be snapshotted\"],"
                    "\"dropped_draws\":%" PRIu64 ",\"dropped_resources\":%" PRIu64 "}",
                c->dropped_draws, c->dropped_resources);
    }
    tb_puts(tb, "}");

    return 1;
}

/* ---------------------------------------------------------------------------------------------------------- *
 * Writing.
 * ---------------------------------------------------------------------------------------------------------- */

static int fetch_shaders(scenecap_capture *c) {
    for (size_t i = 0; i < c->pipe_count; i++) {
        pipe_rec *p = &c->pipes[i];
        for (uint32_t s = 0; s < p->stage_count; s++) {
            stage_rec *st = &p->stages[s];
            const char *text = NULL;
            size_t len = 0;
            if (c->host.shader_source && c->host.shader_source(c->host.user, st->key, &text, &len) && text && len &&
                len <= SC_SHADER_LIMIT) {
                st->msl = malloc(len);
                if (st->msl) {
                    memcpy(st->msl, text, len);
                    st->msl_size = len;
                    st->have_msl = 1;
                }
            }
            text = NULL;
            len = 0;
            if (c->host.shader_reflection && c->host.shader_reflection(c->host.user, st->key, &text, &len) && text &&
                len && len <= SC_SHADER_LIMIT) {
                st->refl = malloc(len);
                if (st->refl) {
                    memcpy(st->refl, text, len);
                    st->refl_size = len;
                    st->have_refl = 1;
                }
            }
            if (!st->have_msl)
                sc_log(c, "scenecap: no MSL for shader %s; draws using it are dropped", st->key);
        }
    }
    return 1;
}

/* Linear bytes-per-texel for formats the offline replayer understands. Block formats return 0 here and are
 * sized by texture_snapshot_bytes() instead. depth32float_stencil8 is snapshotted as depth-only float32 (4 B). */
static uint32_t texture_bytes_per_texel(const char *format, uint32_t sample_count) {
    static const struct {
        const char *name;
        uint32_t bytes;
    } table[] = {
        {"r8unorm", 1},           {"rg8unorm", 2},         {"rgba8unorm", 4},        {"rgba8unorm_srgb", 4},
        {"bgra8unorm", 4},        {"bgra8unorm_srgb", 4},  {"rgba8snorm", 4},        {"r32float", 4},
        {"rgba16unorm", 8},       {"rgba16float", 8},      {"rg16float", 4},         {"rg32float", 8},
        {"rgba32float", 16},      {"rgb10a2unorm", 4},     {"bgr10a2unorm", 4},      {"depth16unorm", 2},
        {"depth32float", 4},      {"depth24_stencil8", 4}, {"depth32float_stencil8", 4}, {"stencil8", 1},
        {"rg11b10float", 4},      {"r16float", 2},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (format && strcmp(format, table[i].name) == 0) return table[i].bytes;
    return sample_count > 1 ? 8 : 4;
}

/* Returns 1 and fills (bw,bh,bbs) for BC/ASTC names the host may emit; 0 otherwise. */
static int texture_block_layout(const char *format, uint32_t *bw, uint32_t *bh, uint32_t *bbs) {
    static const struct {
        const char *name;
        uint32_t bw, bh, bbs;
    } table[] = {
        {"bc1_rgba", 4, 4, 8},      {"bc1_rgba_srgb", 4, 4, 8},
        {"bc2_rgba", 4, 4, 16},     {"bc2_rgba_srgb", 4, 4, 16},
        {"bc3_rgba", 4, 4, 16},     {"bc3_rgba_srgb", 4, 4, 16},
        {"bc4_runorm", 4, 4, 8},    {"bc5_rgunorm", 4, 4, 16},
        {"bc7_rgbaunorm", 4, 4, 16},{"bc7_rgbaunorm_srgb", 4, 4, 16},
        {"astc_4x4", 4, 4, 16},     {"astc_4x4_srgb", 4, 4, 16},
        {"astc_5x5", 5, 5, 16},     {"astc_5x5_srgb", 5, 5, 16},
        {"astc_6x6", 6, 6, 16},     {"astc_6x6_srgb", 6, 6, 16},
        {"astc_8x8", 8, 8, 16},     {"astc_8x8_srgb", 8, 8, 16},
        {"astc_10x10", 10, 10, 16}, {"astc_10x10_srgb", 10, 10, 16},
        {"astc_12x12", 12, 12, 16}, {"astc_12x12_srgb", 12, 12, 16},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (format && strcmp(format, table[i].name) == 0) {
            *bw = table[i].bw;
            *bh = table[i].bh;
            *bbs = table[i].bbs;
            return 1;
        }
    return 0;
}

static uint64_t texture_snapshot_bytes(const scenecap_texture_info *info, uint32_t width, uint32_t height) {
    uint32_t bw, bh, bbs;
    if (texture_block_layout(info->pixel_format, &bw, &bh, &bbs)) {
        uint64_t rows = ((uint64_t)width + bw - 1) / bw;
        uint64_t cols = ((uint64_t)height + bh - 1) / bh;
        uint64_t need = rows * cols * bbs;
        if (info->texture_type == 2) need *= info->depth ? info->depth : 1;
        return need;
    }
    uint32_t bpp = texture_bytes_per_texel(info->pixel_format, info->sample_count);
    if (info->sample_count > 1) bpp = 8;
    uint64_t need = (uint64_t)width * height * bpp;
    if (info->texture_type == 2) need *= info->depth ? info->depth : 1;
    return need;
}

static void fetch_textures(scenecap_capture *c) {
    uint64_t total = c->opt.max_total_bytes ? c->opt.max_total_bytes : SC_DEFAULT_TOTAL_LIMIT;
    uint64_t limit = c->opt.max_texture_bytes ? c->opt.max_texture_bytes : SC_DEFAULT_TEXTURE_LIMIT;
    for (size_t i = 0; i < c->res_count; i++) {
        res_rec *r = &c->res[i];
        if (!r->is_texture || r->skipped) continue;
        if (!c->host.texture_read) {
            r->skipped = 1;
            continue;
        }
        uint32_t width = r->info.width >> (r->level < 32 ? r->level : 31);
        uint32_t height = r->info.height >> (r->level < 32 ? r->level : 31);
        if (!width) width = 1;
        if (!height) height = 1;
        uint64_t need = texture_snapshot_bytes(&r->info, width, height);
        if (need > limit || c->total_bytes + need > total) {
            r->skipped = 1;
            c->dropped_resources++;
            sc_log(c, "scenecap: texture serial %" PRIu64 " needs %" PRIu64 " bytes; over the capture budget",
                   r->serial, need);
            continue;
        }
        uint8_t *copy = malloc((size_t)need);
        if (!copy) {
            r->skipped = 1;
            continue;
        }
        long got = c->host.texture_read(c->host.user, r->serial, r->level, r->layer, copy, (size_t)need);
        if (got <= 0) {
            free(copy);
            r->skipped = 1;
            c->dropped_resources++;
            continue;
        }
        r->data = copy;
        r->data_size = (size_t)got;
        r->info.width = width;
        r->info.height = height;
        c->total_bytes += (uint64_t)got;
    }
}

static int add_blob(scenecap_capture *c, nvs_writer *writer, const char *id, nvs_kind kind, const void *data,
                    size_t size) {
    for (size_t i = 0; i < c->blob_id_count; i++)
        if (strcmp(c->blob_ids[i], id) == 0) return 1;  /* already stored: one shader can serve many pipelines */
    nvs_blob_input blob;
    nvs_error err;
    memset(&blob, 0, sizeof blob);
    memset(&err, 0, sizeof err);
    blob.id = id;
    blob.kind = kind;
    blob.data = data;
    blob.size = size;
    if (!c->opt.compress) blob.flags = NVS_BLOB_STORE_RAW;
    if (nvs_writer_add_blob(writer, &blob, &err) != NVS_OK) {
        sc_error(c, "cannot add blob %s: %s", id, err.message);
        return 0;
    }
    if (!sc_grow((void **)&c->blob_ids, &c->blob_id_cap, c->blob_id_count, sizeof *c->blob_ids)) {
        sc_error(c, "out of memory while remembering blob %s", id);
        return 0;
    }
    size_t id_size = strlen(id) + 1;
    char *id_copy = malloc(id_size);
    if (!id_copy) {
        sc_error(c, "out of memory while remembering blob %s", id);
        return 0;
    }
    memcpy(id_copy, id, id_size);
    c->blob_ids[c->blob_id_count++] = id_copy;
    return 1;
}

int scenecap_finish(scenecap_capture *c, char *err, size_t err_size) {
    if (!c) return 0;
    if (c->state != 1) {
        sc_error(c, "the capture window is not open");
        goto fail;
    }
    c->state = 2;
    fetch_shaders(c);
    fetch_textures(c);

    if (!c->pkg_path[0]) {
        const char *pattern = c->opt.output_path && c->opt.output_path[0] ? c->opt.output_path
                                                                         : "scenecap_%s_f%llu.nvs";
        char expanded[1024];
        char frame_text[32];
        snprintf(frame_text, sizeof frame_text, "%" PRIu64, c->first_frame);
        const char *percent = strstr(pattern, "%llu");
        if (percent) {
            size_t head = (size_t)(percent - pattern);
            size_t tail = strlen(percent + 4);
            if (head + strlen(frame_text) + tail + 1 > sizeof expanded) head = 0;
            memcpy(expanded, pattern, head);
            memcpy(expanded + head, frame_text, strlen(frame_text));
            memcpy(expanded + head + strlen(frame_text), percent + 4, tail + 1);
        } else {
            char *hash = strchr(pattern, '#');
            if (hash) {
                size_t head = (size_t)(hash - pattern);
                snprintf(expanded, sizeof expanded, "%.*s%s%s", (int)head, pattern, frame_text, hash + 1);
            } else {
                snprintf(expanded, sizeof expanded, "%s", pattern);
            }
        }
        char substituted[1024];
        const char *percent_s = strstr(expanded, "%s");
        if (percent_s) {
            const char *game = c->opt.game && c->opt.game[0] ? c->opt.game : "scene";
            size_t head = (size_t)(percent_s - expanded);
            snprintf(substituted, sizeof substituted, "%.*s%s%s", (int)head, expanded, game, percent_s + 2);
            snprintf(c->pkg_path, sizeof c->pkg_path, "%s", substituted);
        } else {
            snprintf(c->pkg_path, sizeof c->pkg_path, "%s", expanded);
        }
    }

    text_buf manifest;
    tb_init(&manifest);
    build_manifest(c, &manifest);
    if (manifest.oom) {
        tb_free(&manifest);
        sc_error(c, "out of memory while building the manifest");
        goto fail;
    }

    {
        int any = 0;
        for (size_t i = 0; i < c->pass_count; i++) any |= c->passes[i].kept;
        if (!any) {
            tb_free(&manifest);
            sc_error(c, "nothing usable was captured: no pass had a snapshot of its attachments or pipeline");
            goto fail;
        }
    }

    nvs_write_options write_options;
    nvs_error nerr;
    memset(&write_options, 0, sizeof write_options);
    memset(&nerr, 0, sizeof nerr);
    write_options.generator = c->opt.generator && c->opt.generator[0] ? c->opt.generator : "scenecap-capture/1.0";
    nvs_writer *writer = nvs_writer_create(c->pkg_path, &write_options, &nerr);
    if (!writer) {
        tb_free(&manifest);
        sc_error(c, "cannot create %s: %s", c->pkg_path, nerr.message);
        goto fail;
    }

    for (size_t i = 0; i < c->pipe_count; i++) {
        if (!pipe_usable(c, (int32_t)i)) continue;
        pipe_rec *p = &c->pipes[i];
        for (uint32_t s = 0; s < p->stage_count; s++) {
            stage_rec *st = &p->stages[s];
            if (!add_blob(c, writer, st->msl_id, NVS_KIND_MSL, st->msl, st->msl_size)) {
                nvs_writer_finish(writer, &nerr);
                tb_free(&manifest);
                goto fail;
            }
            if (st->have_refl && !add_blob(c, writer, st->refl_id, NVS_KIND_REFLECTION, st->refl, st->refl_size)) {
                nvs_writer_finish(writer, &nerr);
                tb_free(&manifest);
                goto fail;
            }
        }
    }
    for (size_t i = 0; i < c->res_count; i++) {
        res_rec *r = &c->res[i];
        if (!r->used) continue;
        if (!add_blob(c, writer, r->id, r->is_texture ? NVS_KIND_TEXTURE : NVS_KIND_BUFFER, r->data, r->data_size)) {
            nvs_writer_finish(writer, &nerr);
            tb_free(&manifest);
            goto fail;
        }
    }
    if (nvs_writer_set_manifest(writer, manifest.data ? manifest.data : "{}", manifest.len, &nerr) != NVS_OK) {
        char why[512];
        snprintf(why, sizeof why, "%s", nerr.message);
        nvs_writer_finish(writer, &nerr);
        tb_free(&manifest);
        sc_error(c, "cannot set the manifest: %s", why);
        goto fail;
    }
    tb_free(&manifest);
    if (nvs_writer_finish(writer, &nerr) != NVS_OK) {
        sc_error(c, "cannot write %s: %s", c->pkg_path, nerr.message);
        goto fail;
    }

    /* Self-check: reopen the package and hash every block, so a broken capture never looks like a good one. */
    {
        nvs_reader *reader = NULL;
        int open_code = nvs_read_open(c->pkg_path, &reader, &nerr);
        if (open_code != NVS_OK || nvs_read_verify_hashes(reader, &nerr) != NVS_OK) {
            sc_error(c, "self-check of %s failed: %s", c->pkg_path, nerr.message);
            nvs_read_close(reader);
            goto fail;
        }
        nvs_read_close(reader);
    }

    c->wrote = 1;
    {
        size_t used = 0;
        for (size_t i = 0; i < c->res_count; i++)
            if (c->res[i].used) used++;
        sc_log(c, "scenecap: wrote %s (%zu pass(es), %zu draw(s), %zu resource(s))", c->pkg_path, c->pass_count,
               c->draw_count, used);
    }
    if (err && err_size) snprintf(err, err_size, "%s", c->pkg_path);
    return 1;

fail:
    if (c->state != 2) c->state = 3;
    else c->state = 3;
    if (err && err_size) snprintf(err, err_size, "%s", c->err);
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------- *
 * Lifecycle.
 * ---------------------------------------------------------------------------------------------------------- */

void scenecap_destroy(scenecap_capture *c) {
    if (!c) return;
    for (size_t i = 0; i < c->res_count; i++) free(c->res[i].data);
    for (size_t i = 0; i < c->pipe_count; i++) {
        for (uint32_t s = 0; s < c->pipes[i].stage_count; s++) {
            free(c->pipes[i].stages[s].msl);
            free(c->pipes[i].stages[s].refl);
        }
    }
    for (size_t i = 0; i < c->blob_id_count; i++) free(c->blob_ids[i]);
    free(c->blob_ids);
    for (size_t i = 0; i < c->pass_count; i++) {
        free(c->passes[i].draws);
        free(c->passes[i].pipes);
    }
    free(c->res);
    free(c->pipes);
    free(c->passes);
    free(c->draws);
    free(c->dispatches);
    free(c->binds);
    free(c);
}

scenecap_capture *scenecap_create(const scenecap_host *host, const scenecap_options *options, char *err,
                                  size_t err_size) {
    if (!host || !options) {
        if (err && err_size) snprintf(err, err_size, "scenecap_create: missing host or options");
        return NULL;
    }
    scenecap_capture *c = calloc(1, sizeof *c);
    if (!c) {
        if (err && err_size) snprintf(err, err_size, "scenecap_create: out of memory");
        return NULL;
    }
    c->host = *host;
    c->opt = *options;
    c->first_frame = options->first_frame;
    c->frame_count = options->frame_count ? options->frame_count : 1;
    c->state = 0;
    c->open_pass = -1;
    return c;
}

int scenecap_options_from_env(scenecap_options *options) {
    if (!options) return 0;
    memset(options, 0, sizeof *options);
    options->compress = 0;
    options->max_texture_bytes = SC_DEFAULT_TEXTURE_LIMIT;
    options->max_buffer_bytes = SC_DEFAULT_BUFFER_LIMIT;
    options->max_total_bytes = SC_DEFAULT_TOTAL_LIMIT;
    /* Budget / compress / first-frame apply to command-triggered captures too (F9, `scenecap <path>`,
       control socket). Previously they were gated behind SCENECAP=1, so a courtyard capture with
       SCENECAP_MAX_BYTES=8G still used the 512 MiB default and dropped every body VB. */
    const char *budget = getenv("SCENECAP_MAX_BYTES");
    if (budget && budget[0]) {
        long long value = strtoll(budget, NULL, 0);
        if (value > 0) options->max_total_bytes = (uint64_t)value;
    }
    const char *buf_lim = getenv("SCENECAP_MAX_BUFFER_BYTES");
    if (buf_lim && buf_lim[0]) {
        long long value = strtoll(buf_lim, NULL, 0);
        if (value > 0) options->max_buffer_bytes = (uint64_t)value;
    }
    const char *tex_lim = getenv("SCENECAP_MAX_TEXTURE_BYTES");
    if (tex_lim && tex_lim[0]) {
        long long value = strtoll(tex_lim, NULL, 0);
        if (value > 0) options->max_texture_bytes = (uint64_t)value;
    }
    const char *compress = getenv("SCENECAP_COMPRESS");
    if (compress && compress[0] && strcmp(compress, "0") != 0) options->compress = 1;
    const char *first = getenv("SCENECAP_FIRST_FRAME");
    if (first && first[0]) {
        long long value = strtoll(first, NULL, 0);
        if (value > 0) options->first_frame = (uint64_t)value;
    }
    const char *enabled = getenv("SCENECAP");
    if (!enabled || !enabled[0] || strcmp(enabled, "0") == 0) return 0;
    options->output_path = getenv("SCENECAP_PKG");
    const char *frames = getenv("SCENECAP_FRAMES");
    if (frames && frames[0]) {
        const char *colon = strchr(frames, ':');
        if (colon) {
            long long first_f = strtoll(frames, NULL, 0);
            long count = strtol(colon + 1, NULL, 0);
            if (first_f > 0) options->first_frame = (uint64_t)first_f;
            options->frame_count = count > 0 ? (uint32_t)count : 1;
        } else {
            long count = strtol(frames, NULL, 0);
            options->frame_count = count > 0 ? (uint32_t)count : 1;
        }
    }
    /* An env-only start (SCENECAP=1 without SCENECAP_FRAMES) used to leave frame_count at 0, which
       made the window [first, first) -- the recorder armed and finished in the same frame boundary,
       writing an empty package before the caller's `scenecap <path>` command could re-arm it. */
    if (!options->frame_count) options->frame_count = 1;
    return 1;
}

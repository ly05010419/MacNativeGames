/* SPDX-License-Identifier: MIT
 *
 * Reads Link's skeleton and skinned triangles out of one decompressed FRES 10.4 model file
 * (Ocarina of Time, Switch 2 edition). Pure C11: no allocation, no I/O, no runtime dependency.
 *
 * The input is game data and is not trusted: every offset is checked before it is read. Functions
 * that can fail return NULL on success and a static error string otherwise. Views returned by this
 * header (names, oot_bfres_shape) point into the caller's buffer and oot_bfres, which must outlive them.
 *
 * Matrix palette, as the model's vertices index it: the smooth matrices first, then the rigid ones.
 *   palette[k]             k < smooth_count   world(bone) x inverse rest(k)   (vertices in model space)
 *   palette[smooth + r]                       world(bone)                     (vertices in bone space)
 * oot_bfres_open refuses a file laid out in any other order. Matrices are 3x4, row major, 12 floats.
 */
#ifndef OOT_BFRES_H
#define OOT_BFRES_H

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define OOT_BFRES_VERSION 0x000a0400u
#define OOT_BFRES_MAX_SKIN 8u
#define OOT_BFRES_HASH_BYTES 1024u
#define OOT_BFRES_TRIANGLE_LIST 3u
#define OOT_BFRES_MIN_WEIGHT 0.5
/* Words of the bone set that oot_bfres_boneset_* and oot_bfres_mark_triangles use. */
#define OOT_BFRES_BONESET_WORDS(bones) (((size_t)(bones) + 31u) / 32u)

enum {
    OOT_BFRES__HEADER_SIZE = 0xc0,
    OOT_BFRES__POOL_INFO_SIZE = 0x10,
    OOT_BFRES__MODEL_SIZE = 0x70,
    OOT_BFRES__SKELETON_SIZE = 0x40,
    OOT_BFRES__BONE_SIZE = 0x58,
    OOT_BFRES__SHAPE_SIZE = 0x60,
    OOT_BFRES__VERTEX_SIZE = 0x58,
    OOT_BFRES__ATTRIBUTE_SIZE = 0x10,
    OOT_BFRES__BUFFER_ENTRY_SIZE = 0x10,
    OOT_BFRES__MESH_SIZE = 0x38,
    OOT_BFRES__MATRIX_SIZE = 48,
    OOT_BFRES__ROTATION_MASK = 0x7000,
    OOT_BFRES__ROTATION_EULER_XYZ = 0x1000,
    OOT_BFRES__TYPE_UNORM = 1,
    OOT_BFRES__TYPE_UINT = 3
};

typedef struct {
    const uint8_t *data;
    size_t size;
    uint64_t pool, pool_size;
    uint64_t bones, matrix_to_bone, inverse; /* inverse == 0: the file stores none */
    uint64_t shapes;
    unsigned bone_count, smooth_count, rigid_count, shape_count;
} oot_bfres;

typedef struct {
    const char *name; /* NUL-terminated, inside the file */
    size_t name_len;
    int parent, smooth, rigid; /* -1 = none; smooth and rigid are palette indices */
    float scale[3], rotation[3], translation[3]; /* rotation: Euler XYZ radians, R = Rz Ry Rx */
} oot_bfres_bone;

typedef struct {
    unsigned present, offset, components, bytes;
} oot_bfres_attribute;

typedef struct {
    const oot_bfres *model;
    const char *name;
    size_t name_len;
    unsigned skin_count, vertex_count;
    unsigned lod_count, primitive;
    unsigned index_count, index_format, index_size, first_vertex; /* LOD 0; format 0/1/2 = 8/16/32 bit */
    /* FNV-1a 32 of the first min(1024, size) index bytes. Not unique alone: two shapes can share their
     * first 1024 bytes, or a whole index buffer, so match it together with index_count and check for ties. */
    uint32_t index_hash;
    const uint8_t *indices;
    const uint8_t *vertices;
    unsigned stride;
    oot_bfres_attribute bone_attr[2], weight_attr[2]; /* _i0 _i1, _w0 _w1 */
} oot_bfres_shape;

/* One vertex's influences; entries with a zero weight are left out. */
typedef struct {
    unsigned count;
    uint16_t matrix[OOT_BFRES_MAX_SKIN]; /* palette index */
    uint16_t bone[OOT_BFRES_MAX_SKIN];
    float weight[OOT_BFRES_MAX_SKIN];
} oot_bfres_skin;

static inline int oot_bfres__has(const oot_bfres *m, uint64_t offset, uint64_t length)
{
    return offset <= m->size && length <= m->size - offset;
}

static inline int oot_bfres__in_pool(const oot_bfres *m, uint64_t offset, uint64_t length)
{
    return offset <= m->pool_size && length <= m->pool_size - offset;
}

static inline uint32_t oot_bfres__u16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static inline uint32_t oot_bfres__u32(const uint8_t *p)
{
    return oot_bfres__u16(p) | oot_bfres__u16(p + 2) << 16;
}

static inline uint64_t oot_bfres__u64(const uint8_t *p)
{
    return (uint64_t)oot_bfres__u32(p) | (uint64_t)oot_bfres__u32(p + 4) << 32;
}

static inline int oot_bfres__s16(const uint8_t *p)
{
    uint32_t v = oot_bfres__u16(p);
    return v >= 0x8000u ? (int)v - 0x10000 : (int)v;
}

static inline float oot_bfres__f32(const uint8_t *p)
{
    uint32_t bits = oot_bfres__u32(p);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static inline uint32_t oot_bfres_fnv1a32(const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t hash = 0x811c9dc5u;
    for (size_t i = 0; i < size; i++)
        hash = (hash ^ p[i]) * 0x01000193u;
    return hash;
}

/* Names are stored as a 16-bit length, the characters, and a NUL. */
static inline int oot_bfres__name(const oot_bfres *m, uint64_t at, const char **name, size_t *length)
{
    if (!oot_bfres__has(m, at, 2))
        return 0;
    size_t n = oot_bfres__u16(m->data + at);
    if (!oot_bfres__has(m, at + 2, (uint64_t)n + 1) || m->data[at + 2 + n] != 0)
        return 0;
    *name = (const char *)m->data + at + 2;
    *length = n;
    return 1;
}

static inline const char *oot_bfres__open_header(oot_bfres *m)
{
    const uint8_t *d = m->data;
    if (!oot_bfres__has(m, 0, OOT_BFRES__HEADER_SIZE))
        return "bfres: shorter than the file header";
    if (memcmp(d, "FRES    ", 8) != 0)
        return "bfres: not a FRES file";
    if (oot_bfres__u32(d + 8) != OOT_BFRES_VERSION)
        return "bfres: not FRES version 10.4";
    if (d[12] != 0xff || d[13] != 0xfe)
        return "bfres: not little-endian";
    uint64_t info = oot_bfres__u64(d + 0xb0);
    if (!oot_bfres__has(m, info, OOT_BFRES__POOL_INFO_SIZE))
        return "bfres: buffer pool record outside the file";
    m->pool_size = oot_bfres__u32(d + info + 4);
    m->pool = oot_bfres__u64(d + info + 8);
    if (!oot_bfres__has(m, m->pool, m->pool_size))
        return "bfres: buffer pool outside the file";
    uint64_t model = oot_bfres__u64(d + 0x28);
    if (!oot_bfres__has(m, model, OOT_BFRES__MODEL_SIZE) || memcmp(d + model, "FMDL", 4) != 0)
        return "bfres: model 0 missing";
    m->shapes = oot_bfres__u64(d + model + 0x28);
    m->shape_count = oot_bfres__u16(d + model + 0x6a);
    if (!oot_bfres__has(m, m->shapes, (uint64_t)m->shape_count * OOT_BFRES__SHAPE_SIZE))
        return "bfres: shape table outside the file";
    uint64_t skeleton = oot_bfres__u64(d + model + 0x18);
    if (!oot_bfres__has(m, skeleton, OOT_BFRES__SKELETON_SIZE) || memcmp(d + skeleton, "FSKL", 4) != 0)
        return "bfres: skeleton missing";
    if ((oot_bfres__u32(d + skeleton + 4) & OOT_BFRES__ROTATION_MASK) != OOT_BFRES__ROTATION_EULER_XYZ)
        return "bfres: skeleton rotations are not Euler XYZ";
    m->bones = oot_bfres__u64(d + skeleton + 0x10);
    m->matrix_to_bone = oot_bfres__u64(d + skeleton + 0x18);
    m->inverse = oot_bfres__u64(d + skeleton + 0x20);
    m->bone_count = oot_bfres__u16(d + skeleton + 0x38);
    m->smooth_count = oot_bfres__u16(d + skeleton + 0x3a);
    m->rigid_count = oot_bfres__u16(d + skeleton + 0x3c);
    return NULL;
}

static inline const char *oot_bfres__check_tables(const oot_bfres *m)
{
    uint64_t matrices = (uint64_t)m->smooth_count + m->rigid_count;
    if (m->bone_count == 0)
        return "bfres: skeleton has no bones";
    if (!oot_bfres__has(m, m->bones, (uint64_t)m->bone_count * OOT_BFRES__BONE_SIZE))
        return "bfres: bone table outside the file";
    if (!oot_bfres__has(m, m->matrix_to_bone, matrices * 2))
        return "bfres: matrix table outside the file";
    if (m->inverse && !oot_bfres__has(m, m->inverse, (uint64_t)m->smooth_count * OOT_BFRES__MATRIX_SIZE))
        return "bfres: inverse matrices outside the file";
    return NULL;
}

/* A bone's palette entry must point back at it, inside its half of the palette. */
static inline int oot_bfres__entry_ok(const oot_bfres *m, int entry, unsigned low, unsigned count, unsigned bone)
{
    if (entry < 0)
        return entry == -1;
    if ((unsigned)entry < low || (unsigned)entry - low >= count)
        return 0;
    return oot_bfres__u16(m->data + m->matrix_to_bone + (uint64_t)entry * 2) == bone;
}

static inline const char *oot_bfres__check_bones(const oot_bfres *m)
{
    unsigned smooth = 0, rigid = 0;
    for (unsigned i = 0; i < m->bone_count; i++) {
        const uint8_t *b = m->data + m->bones + (uint64_t)i * OOT_BFRES__BONE_SIZE;
        const char *name;
        size_t length;
        if (!oot_bfres__name(m, oot_bfres__u64(b), &name, &length))
            return "bfres: bone name outside the file";
        if (oot_bfres__u16(b + 0x20) != i)
            return "bfres: bone index does not match its place";
        int parent = oot_bfres__s16(b + 0x22), s = oot_bfres__s16(b + 0x24), r = oot_bfres__s16(b + 0x26);
        if (parent < -1 || parent >= (int)i)
            return "bfres: bone parent does not come before it";
        if (!oot_bfres__entry_ok(m, s, 0, m->smooth_count, i) ||
            !oot_bfres__entry_ok(m, r, m->smooth_count, m->rigid_count, i))
            return "bfres: palette is not smooth then rigid, or does not match its bones";
        smooth += s >= 0;
        rigid += r >= 0;
    }
    if (smooth != m->smooth_count || rigid != m->rigid_count)
        return "bfres: palette entries without a bone";
    return NULL;
}

/* Opens model 0 and validates its skeleton. */
static inline const char *oot_bfres_open(oot_bfres *out, const void *data, size_t size)
{
    oot_bfres m;
    if (!out || !data)
        return "bfres: null argument";
    memset(&m, 0, sizeof m);
    m.data = (const uint8_t *)data;
    m.size = size;
    const char *error = oot_bfres__open_header(&m);
    if (!error)
        error = oot_bfres__check_tables(&m);
    if (!error)
        error = oot_bfres__check_bones(&m);
    if (error)
        return error;
    *out = m;
    return NULL;
}

static inline unsigned oot_bfres_matrix_count(const oot_bfres *m)
{
    return m->smooth_count + m->rigid_count;
}

/* Palette index -> bone index; -1 when out of range. Smooth k is palette k, rigid r is palette smooth + r. */
static inline int oot_bfres_matrix_bone(const oot_bfres *m, unsigned matrix)
{
    if (matrix >= oot_bfres_matrix_count(m))
        return -1;
    return (int)oot_bfres__u16(m->data + m->matrix_to_bone + (uint64_t)matrix * 2);
}

static inline int oot_bfres_smooth_bone(const oot_bfres *m, unsigned smooth)
{
    return smooth < m->smooth_count ? oot_bfres_matrix_bone(m, smooth) : -1;
}

static inline int oot_bfres_rigid_bone(const oot_bfres *m, unsigned rigid)
{
    return rigid < m->rigid_count ? oot_bfres_matrix_bone(m, m->smooth_count + rigid) : -1;
}

static inline const char *oot_bfres_bone_at(const oot_bfres *m, unsigned index, oot_bfres_bone *out)
{
    if (index >= m->bone_count)
        return "bfres: bone index out of range";
    const uint8_t *b = m->data + m->bones + (uint64_t)index * OOT_BFRES__BONE_SIZE;
    oot_bfres_bone bone;
    if (!oot_bfres__name(m, oot_bfres__u64(b), &bone.name, &bone.name_len))
        return "bfres: bone name outside the file";
    bone.parent = oot_bfres__s16(b + 0x22);
    bone.smooth = oot_bfres__s16(b + 0x24);
    bone.rigid = oot_bfres__s16(b + 0x26);
    for (unsigned axis = 0; axis < 3; axis++) {
        bone.scale[axis] = oot_bfres__f32(b + 0x30 + axis * 4);
        bone.rotation[axis] = oot_bfres__f32(b + 0x3c + axis * 4);
        bone.translation[axis] = oot_bfres__f32(b + 0x4c + axis * 4);
    }
    *out = bone;
    return NULL;
}

/* The bone called name, or -1. */
static inline int oot_bfres_find_bone(const oot_bfres *m, const char *name)
{
    size_t wanted = strlen(name);
    for (unsigned i = 0; i < m->bone_count; i++) {
        oot_bfres_bone bone;
        if (!oot_bfres_bone_at(m, i, &bone) && bone.name_len == wanted && !memcmp(bone.name, name, wanted))
            return (int)i;
    }
    return -1;
}

static inline int oot_bfres__store(double value, float *out)
{
    if (!(fabs(value) <= (double)FLT_MAX))
        return 0;
    *out = (float)value;
    return 1;
}

/* out = a x b for 3x4 matrices; 0 when a result is not a finite float. */
static inline int oot_bfres__multiply(const float a[12], const float b[12], float out[12])
{
    float result[12];
    for (unsigned row = 0; row < 3; row++) {
        const float *r = a + row * 4;
        for (unsigned column = 0; column < 4; column++) {
            double value = (double)r[0] * b[column] + (double)r[1] * b[4 + column] + (double)r[2] * b[8 + column];
            if (column == 3)
                value += r[3];
            if (!oot_bfres__store(value, &result[row * 4 + column]))
                return 0;
        }
    }
    memcpy(out, result, sizeof result);
    return 1;
}

static inline int oot_bfres__local(const oot_bfres_bone *bone, float out[12])
{
    double cx = cos(bone->rotation[0]), sx = sin(bone->rotation[0]);
    double cy = cos(bone->rotation[1]), sy = sin(bone->rotation[1]);
    double cz = cos(bone->rotation[2]), sz = sin(bone->rotation[2]);
    const double r[9] = {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx,
                         sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx,
                         -sy,     cy * sx,                cy * cx};
    for (unsigned row = 0; row < 3; row++) {
        for (unsigned column = 0; column < 3; column++)
            if (!oot_bfres__store(r[row * 3 + column] * bone->scale[column], &out[row * 4 + column]))
                return 0;
        if (!oot_bfres__store(bone->translation[row], &out[row * 4 + 3]))
            return 0;
    }
    return 1;
}

static inline int oot_bfres__unit_scale(const oot_bfres_bone *bone)
{
    return bone->scale[0] == 1.0f && bone->scale[1] == 1.0f && bone->scale[2] == 1.0f;
}

/* World rest pose of every bone: world holds bone_count x 12 floats. Children of a scaled bone are
 * refused, because how scale passes to children is not established by the files this reads. */
static inline const char *oot_bfres_rest_world(const oot_bfres *m, float *world, size_t world_matrices)
{
    if (!world || world_matrices < m->bone_count)
        return "bfres: world array too small";
    for (unsigned i = 0; i < m->bone_count; i++) {
        oot_bfres_bone bone;
        float local[12];
        const char *error = oot_bfres_bone_at(m, i, &bone);
        if (error)
            return error;
        if (!oot_bfres__local(&bone, local))
            return "bfres: bone transform is not finite";
        if (bone.parent < 0) {
            memcpy(world + (size_t)i * 12, local, sizeof local);
            continue;
        }
        oot_bfres_bone parent;
        error = oot_bfres_bone_at(m, (unsigned)bone.parent, &parent);
        if (error)
            return error;
        if (!oot_bfres__unit_scale(&parent))
            return "bfres: bone under a scaled parent is not supported";
        if (!oot_bfres__multiply(world + (size_t)bone.parent * 12, local, world + (size_t)i * 12))
            return "bfres: bone transform is not finite";
    }
    return NULL;
}

static inline int oot_bfres_has_inverse(const oot_bfres *m)
{
    return m->inverse != 0;
}

/* The stored inverse rest matrix of smooth entry k. */
static inline const char *oot_bfres_inverse_rest(const oot_bfres *m, unsigned smooth, float out[12])
{
    if (!oot_bfres_has_inverse(m))
        return "bfres: the file stores no inverse matrices";
    if (smooth >= m->smooth_count)
        return "bfres: smooth index out of range";
    const uint8_t *p = m->data + m->inverse + (uint64_t)smooth * OOT_BFRES__MATRIX_SIZE;
    for (unsigned i = 0; i < 12; i++)
        out[i] = oot_bfres__f32(p + i * 4);
    return NULL;
}

/* Builds the GPU palette from bone world matrices (bone_count x 12 floats): smooth entries are
 * world x inverse rest, rigid entries are world. palette holds oot_bfres_matrix_count() x 12 floats. */
static inline const char *oot_bfres_palette(const oot_bfres *m, const float *world, float *palette,
                                            size_t palette_matrices)
{
    if (!world || !palette || palette_matrices < oot_bfres_matrix_count(m))
        return "bfres: palette array too small";
    for (unsigned k = 0; k < oot_bfres_matrix_count(m); k++) {
        const float *source = world + (size_t)(unsigned)oot_bfres_matrix_bone(m, k) * 12;
        float *target = palette + (size_t)k * 12;
        if (k >= m->smooth_count) {
            memcpy(target, source, 12 * sizeof(float));
            continue;
        }
        float inverse[12];
        const char *error = oot_bfres_inverse_rest(m, k, inverse);
        if (error)
            return error;
        if (!oot_bfres__multiply(source, inverse, target))
            return "bfres: palette matrix is not finite";
    }
    return NULL;
}

/* Channel layouts seen on skin attributes; anything else is refused rather than guessed. */
static inline int oot_bfres__layout(unsigned channels, unsigned *components, unsigned *bytes)
{
    switch (channels) {
    case 0x02: *components = 1; *bytes = 1; return 1; /* R8 */
    case 0x09: *components = 2; *bytes = 1; return 1; /* R8 G8 */
    case 0x0a: *components = 1; *bytes = 2; return 1; /* R16 */
    case 0x0b: *components = 4; *bytes = 1; return 1; /* R8 G8 B8 A8 */
    case 0x12: *components = 2; *bytes = 2; return 1; /* R16 G16 */
    case 0x15: *components = 4; *bytes = 2; return 1; /* R16 G16 B16 A16 */
    default: return 0;
    }
}

static inline const char *oot_bfres__skin_attribute(oot_bfres_shape *s, const char *name, size_t length,
                                                    const uint8_t *record)
{
    if (length != 3 || name[0] != '_' || (name[1] != 'i' && name[1] != 'w') || (name[2] != '0' && name[2] != '1'))
        return NULL;
    int weight = name[1] == 'w';
    oot_bfres_attribute *slot = (weight ? s->weight_attr : s->bone_attr) + (name[2] - '0');
    uint32_t format = oot_bfres__u32(record + 8);
    oot_bfres_attribute a = {1, oot_bfres__u16(record + 0xc), 0, 0};
    if (slot->present)
        return "bfres: skin attribute appears twice";
    if (oot_bfres__u16(record + 0xe) != 0)
        return "bfres: skin attribute is not in the first vertex buffer";
    if ((format >> 16) != 0 || (format & 0xff) != (unsigned)(weight ? OOT_BFRES__TYPE_UNORM : OOT_BFRES__TYPE_UINT) ||
        !oot_bfres__layout(format >> 8, &a.components, &a.bytes))
        return "bfres: unsupported skin attribute format";
    if ((uint64_t)a.offset + (uint64_t)a.components * a.bytes > s->stride)
        return "bfres: skin attribute outside its vertex";
    *slot = a;
    return NULL;
}

static inline const char *oot_bfres__read_attributes(const oot_bfres *m, oot_bfres_shape *s, uint64_t list,
                                                     unsigned count)
{
    if (!oot_bfres__has(m, list, (uint64_t)count * OOT_BFRES__ATTRIBUTE_SIZE))
        return "bfres: attribute table outside the file";
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *record = m->data + list + (uint64_t)i * OOT_BFRES__ATTRIBUTE_SIZE;
        const char *name;
        size_t length;
        if (!oot_bfres__name(m, oot_bfres__u64(record), &name, &length))
            return "bfres: attribute name outside the file";
        const char *error = oot_bfres__skin_attribute(s, name, length, record);
        if (error)
            return error;
    }
    return NULL;
}

/* The components named by _i0/_i1 and _w0/_w1 must cover skin_count, in that order. */
static inline const char *oot_bfres__check_skin(const oot_bfres_shape *s)
{
    unsigned bones = s->bone_attr[0].components + s->bone_attr[1].components;
    unsigned weights = s->weight_attr[0].components + s->weight_attr[1].components;
    if (s->skin_count > OOT_BFRES_MAX_SKIN)
        return "bfres: vertex skin count above 8";
    if (s->bone_attr[1].present && !s->bone_attr[0].present)
        return "bfres: _i1 without _i0";
    if (s->weight_attr[1].present && !s->weight_attr[0].present)
        return "bfres: _w1 without _w0";
    if (s->skin_count && bones < s->skin_count)
        return "bfres: fewer bone indices than the skin count";
    if (s->skin_count > 1 && weights < s->skin_count)
        return "bfres: fewer weights than the skin count";
    return NULL;
}

static inline const char *oot_bfres__read_vertices(const oot_bfres *m, oot_bfres_shape *s, uint64_t at)
{
    const uint8_t *d = m->data;
    if (!oot_bfres__has(m, at, OOT_BFRES__VERTEX_SIZE) || memcmp(d + at, "FVTX", 4) != 0)
        return "bfres: vertex record missing";
    unsigned attributes = d[at + 0x4c], buffers = d[at + 0x4d];
    uint64_t start = oot_bfres__u32(d + at + 0x48);
    uint64_t sizes = oot_bfres__u64(d + at + 0x30), strides = oot_bfres__u64(d + at + 0x38);
    s->vertex_count = oot_bfres__u32(d + at + 0x50);
    if (buffers == 0)
        return "bfres: shape has no vertex buffer";
    if (!oot_bfres__has(m, sizes, OOT_BFRES__BUFFER_ENTRY_SIZE) ||
        !oot_bfres__has(m, strides, OOT_BFRES__BUFFER_ENTRY_SIZE))
        return "bfres: vertex buffer table outside the file";
    uint64_t size = oot_bfres__u32(d + sizes);
    s->stride = oot_bfres__u32(d + strides);
    if (s->stride == 0)
        return "bfres: vertex stride is zero";
    if ((uint64_t)s->vertex_count * s->stride > size || !oot_bfres__in_pool(m, start, size))
        return "bfres: vertex buffer outside the pool";
    s->vertices = d + m->pool + start;
    const char *error = oot_bfres__read_attributes(m, s, oot_bfres__u64(d + at + 8), attributes);
    return error ? error : oot_bfres__check_skin(s);
}

static inline const char *oot_bfres__read_lod0(const oot_bfres *m, oot_bfres_shape *s, uint64_t meshes)
{
    const uint8_t *d = m->data;
    if (s->lod_count == 0)
        return "bfres: shape has no mesh";
    if (!oot_bfres__has(m, meshes, OOT_BFRES__MESH_SIZE))
        return "bfres: mesh record outside the file";
    uint64_t start = oot_bfres__u32(d + meshes + 0x20);
    s->primitive = oot_bfres__u32(d + meshes + 0x24);
    s->index_format = oot_bfres__u32(d + meshes + 0x28);
    s->index_count = oot_bfres__u32(d + meshes + 0x2c);
    s->first_vertex = oot_bfres__u32(d + meshes + 0x30);
    if (s->index_format > 2)
        return "bfres: unknown index format";
    s->index_size = 1u << s->index_format;
    uint64_t bytes = (uint64_t)s->index_count * s->index_size;
    if (!oot_bfres__in_pool(m, start, bytes))
        return "bfres: index buffer outside the pool";
    s->indices = d + m->pool + start;
    s->index_hash = oot_bfres_fnv1a32(s->indices, bytes < OOT_BFRES_HASH_BYTES ? (size_t)bytes : OOT_BFRES_HASH_BYTES);
    return NULL;
}

static inline const char *oot_bfres_shape_at(const oot_bfres *m, unsigned index, oot_bfres_shape *out)
{
    if (index >= m->shape_count)
        return "bfres: shape index out of range";
    const uint8_t *d = m->data;
    uint64_t at = m->shapes + (uint64_t)index * OOT_BFRES__SHAPE_SIZE;
    if (memcmp(d + at, "FSHP", 4) != 0)
        return "bfres: shape record missing";
    oot_bfres_shape s;
    memset(&s, 0, sizeof s);
    s.model = m;
    if (!oot_bfres__name(m, oot_bfres__u64(d + at + 8), &s.name, &s.name_len))
        return "bfres: shape name outside the file";
    s.skin_count = d[at + 0x5a];
    s.lod_count = d[at + 0x5b];
    const char *error = oot_bfres__read_vertices(m, &s, oot_bfres__u64(d + at + 0x10));
    if (!error)
        error = oot_bfres__read_lod0(m, &s, oot_bfres__u64(d + at + 0x18));
    if (error)
        return error;
    *out = s;
    return NULL;
}

/* Component c of a skin attribute across _x0 then _x1, as a raw integer. */
static inline unsigned oot_bfres__component(const oot_bfres_shape *s, const oot_bfres_attribute pair[2],
                                            unsigned vertex, unsigned c, unsigned *bytes)
{
    const oot_bfres_attribute *a = c < pair[0].components ? &pair[0] : &pair[1];
    unsigned in_attribute = c < pair[0].components ? c : c - pair[0].components;
    const uint8_t *p = s->vertices + (size_t)vertex * s->stride + a->offset + in_attribute * a->bytes;
    *bytes = a->bytes;
    return a->bytes == 1 ? p[0] : oot_bfres__u16(p);
}

/* A vertex's bone influences. Only the first skin_count components count: on real files the
 * component after them can hold a non-zero filler. A skin count of 1 without _w0 has weight 1. */
static inline const char *oot_bfres_vertex_skin(const oot_bfres_shape *s, unsigned vertex, oot_bfres_skin *out)
{
    if (vertex >= s->vertex_count)
        return "bfres: vertex index out of range";
    oot_bfres_skin skin;
    memset(&skin, 0, sizeof skin);
    for (unsigned c = 0; c < s->skin_count; c++) {
        unsigned bytes = 1;
        unsigned matrix = oot_bfres__component(s, s->bone_attr, vertex, c, &bytes);
        float weight = 1.0f;
        if (s->weight_attr[0].present) {
            unsigned raw = oot_bfres__component(s, s->weight_attr, vertex, c, &bytes);
            weight = (float)raw / (bytes == 1 ? 255.0f : 65535.0f);
        }
        if (weight == 0.0f)
            continue;
        int bone = oot_bfres_matrix_bone(s->model, matrix);
        if (bone < 0)
            return "bfres: vertex bone index outside the palette";
        skin.matrix[skin.count] = (uint16_t)matrix;
        skin.bone[skin.count] = (uint16_t)bone;
        skin.weight[skin.count] = weight;
        skin.count++;
    }
    *out = skin;
    return NULL;
}

static inline const char *oot_bfres_boneset_add(const oot_bfres *m, uint32_t *set, unsigned bone)
{
    if (bone >= m->bone_count)
        return "bfres: bone index out of range";
    set[bone / 32] |= (uint32_t)1 << (bone % 32);
    return NULL;
}

static inline int oot_bfres_boneset_has(const uint32_t *set, unsigned bone)
{
    return (set[bone / 32] >> (bone % 32)) & 1u;
}

/* Adds root and all its descendants. Parents always come first, so each walk ends. */
static inline const char *oot_bfres_boneset_add_subtree(const oot_bfres *m, uint32_t *set, unsigned root)
{
    const char *error = oot_bfres_boneset_add(m, set, root);
    if (error)
        return error;
    for (unsigned i = root + 1; i < m->bone_count; i++) {
        int at = (int)i;
        while (at > (int)root) {
            oot_bfres_bone bone;
            error = oot_bfres_bone_at(m, (unsigned)at, &bone);
            if (error)
                return error;
            at = bone.parent;
        }
        if (at == (int)root)
            set[i / 32] |= (uint32_t)1 << (i % 32);
    }
    return NULL;
}

/* Whether the vertex's summed weight on the bones in set reaches 0.5. */
static inline const char *oot_bfres_vertex_in_set(const oot_bfres_shape *s, unsigned vertex, const uint32_t *set,
                                                  int *in_set)
{
    oot_bfres_skin skin;
    const char *error = oot_bfres_vertex_skin(s, vertex, &skin);
    if (error)
        return error;
    double sum = 0.0;
    for (unsigned i = 0; i < skin.count; i++)
        if (oot_bfres_boneset_has(set, skin.bone[i]))
            sum += skin.weight[i];
    *in_set = sum >= OOT_BFRES_MIN_WEIGHT;
    return NULL;
}

/* The vertex a LOD 0 index refers to, after the mesh's first-vertex offset. */
static inline const char *oot_bfres_index_vertex(const oot_bfres_shape *s, unsigned i, unsigned *vertex)
{
    if (i >= s->index_count)
        return "bfres: index out of range";
    const uint8_t *p = s->indices + (size_t)i * s->index_size;
    uint64_t value = s->index_size == 1 ? p[0] : s->index_size == 2 ? oot_bfres__u16(p) : oot_bfres__u32(p);
    value += s->first_vertex;
    if (value >= s->vertex_count)
        return "bfres: index beyond the vertices";
    *vertex = (unsigned)value;
    return NULL;
}

/* keep[t] = 1 when all three vertices of LOD 0 triangle t are in the set, else 0.
 * keep holds index_count / 3 bytes; on an error its contents are not meaningful. */
static inline const char *oot_bfres_mark_triangles(const oot_bfres_shape *s, const uint32_t *set, uint8_t *keep,
                                                   size_t keep_count, unsigned *kept)
{
    if (s->primitive != OOT_BFRES_TRIANGLE_LIST || s->index_count % 3 != 0)
        return "bfres: shape is not a triangle list";
    if (!set || !keep || keep_count != s->index_count / 3)
        return "bfres: triangle array has the wrong size";
    unsigned total = 0;
    for (unsigned t = 0; t < s->index_count / 3; t++) {
        int all = 1;
        for (unsigned c = 0; c < 3; c++) {
            unsigned vertex;
            int in_set;
            const char *error = oot_bfres_index_vertex(s, t * 3 + c, &vertex);
            if (!error)
                error = oot_bfres_vertex_in_set(s, vertex, set, &in_set);
            if (error)
                return error;
            all &= in_set;
        }
        keep[t] = (uint8_t)all;
        total += (unsigned)all;
    }
    if (kept)
        *kept = total;
    return NULL;
}

#endif

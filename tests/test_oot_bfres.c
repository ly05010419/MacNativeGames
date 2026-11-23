/* zeldaVR/runtime/oot_bfres.h on a synthetic FRES built here; no game data. */
#include "../zeldaVR/runtime/oot_bfres.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)
#define OK(call) do { const char *error_ = (call); checks++; \
    if (error_) { printf("FAILED line %d: %s\n", __LINE__, error_); exit(1); } } while (0)
#define REFUSED(call) do { checks++; if (!(call)) { printf("NOT REFUSED line %d: %s\n", __LINE__, #call); exit(1); } } while (0)

enum { FILE_SIZE = 0x4000, POOL_AT = 0x2000, BONES = 6, SMOOTH = 5, RIGID = 2, SHAPES = 4, MAX_ATTRS = 4, BIG_INDICES = 600 };

static unsigned long checks;

typedef struct { const char *name; unsigned format, offset; } attr_spec;
typedef struct {
    const char *name;
    unsigned skin, stride, vertices, attr_count;
    attr_spec attrs[MAX_ATTRS];
    const void *vertex_data;
    unsigned index_format, index_count, first_vertex;
    const void *index_data;
} shape_spec;
typedef struct { const char *name; int parent, smooth, rigid; float rz, t[3]; } bone_spec;
/* Where the builder put things, for the refusal tests. */
typedef struct {
    size_t size, skeleton, bones, matrices, inverse, shapes;
    size_t vertex[SHAPES], attrs[SHAPES], strides[SHAPES], mesh[SHAPES], vertex_pool[SHAPES], index_pool[SHAPES];
} layout;

static const bone_spec BONE_SPECS[BONES] = {
    {"Root", -1, 0, -1, 0.0f, {0, 1, 0}},
    {"Clavicle_L", 0, 1, -1, (float)(3.14159265358979 / 2), {1, 0, 0}},
    {"Arm_1_L", 1, 2, -1, 0.0f, {2, 0, 0}},
    {"Wrist_L", 2, 3, -1, 0.0f, {1, 0, 0}},
    {"Spine", 0, 4, 6, 0.0f, {0, 2, 0}}, /* a bone with both a smooth and a rigid matrix */
    {"Eye", 4, -1, 5, 0.0f, {0, 0, 1}},
};
static const uint16_t MATRIX_TO_BONE[SMOOTH + RIGID] = {0, 1, 2, 3, 4, 5, 4};

static const uint8_t ARM_VERTICES[6][4] = {
    {2, 0, 255, 0}, {3, 4, 128, 127}, {2, 4, 127, 128}, {1, 2, 200, 55}, {4, 0, 255, 0}, {3, 0, 255, 0},
};
static const uint16_t ARM_INDICES[12] = {0, 1, 3, 0, 1, 2, 3, 5, 0, 4, 5, 0};
/* Three influences in four-component attributes: the fourth holds filler, as on the real files. */
static const uint8_t FILLER_VERTICES[3][8] = {
    {2, 3, 1, 0xff, 100, 100, 55, 0x7f}, {4, 4, 4, 0xff, 255, 0, 0, 9}, {2, 0, 0, 0xff, 255, 0, 0, 200},
};
static const uint32_t FILLER_INDICES[6] = {0, 1, 2, 0, 2, 2};
static const uint8_t FIVE_VERTICES[2][12] = {
    {0, 1, 2, 3, 6, 0, 10, 20, 30, 40, 155, 0}, {2, 2, 2, 2, 2, 0, 255, 0, 0, 0, 0, 0},
};
static const uint16_t RIGID_VERTICES[4] = {6, 5, 6, 6};
static const uint8_t RIGID_INDICES[3] = {0, 1, 2};
static uint16_t big_indices[BIG_INDICES];

static shape_spec shape_specs(unsigned i)
{
    const shape_spec specs[SHAPES] = {
        {"ArmShape", 2, 4, 6, 2, {{"_i0", 0x0903, 0}, {"_w0", 0x0901, 2}}, ARM_VERTICES, 1, 12, 0, ARM_INDICES},
        {"FillerShape", 3, 8, 3, 2, {{"_i0", 0x0b03, 0}, {"_w0", 0x0b01, 4}}, FILLER_VERTICES, 2, 6, 0, FILLER_INDICES},
        {"FiveShape", 5, 12, 2, 4,
         {{"_i0", 0x0b03, 0}, {"_i1", 0x0a03, 4}, {"_w0", 0x0b01, 6}, {"_w1", 0x0201, 10}},
         FIVE_VERTICES, 1, BIG_INDICES, 0, big_indices},
        {"RigidShape", 1, 2, 4, 1, {{"_i0", 0x0a03, 0}}, RIGID_VERTICES, 0, 3, 1, RIGID_INDICES},
    };
    return specs[i];
}

static void put16(uint8_t *d, size_t at, unsigned v) { d[at] = (uint8_t)v; d[at + 1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *d, size_t at, uint32_t v) { put16(d, at, v & 0xffff); put16(d, at + 2, v >> 16); }
static void put64(uint8_t *d, size_t at, uint64_t v) { put32(d, at, (uint32_t)v); put32(d, at + 4, (uint32_t)(v >> 32)); }
static void putf(uint8_t *d, size_t at, float v) { uint32_t bits; memcpy(&bits, &v, 4); put32(d, at, bits); }

static size_t take(size_t *cursor, size_t size)
{
    size_t at = (*cursor + 7) & ~(size_t)7;
    *cursor = at + size;
    if (*cursor > POOL_AT) { printf("metadata too large\n"); exit(1); }
    return at;
}

static size_t put_name(uint8_t *d, size_t *cursor, const char *name)
{
    size_t length = strlen(name), at = take(cursor, length + 3);
    put16(d, at, (unsigned)length);
    memcpy(d + at + 2, name, length);
    return at;
}

static void build_bones(uint8_t *d, size_t *cursor, const layout *l)
{
    for (unsigned i = 0; i < BONES; i++) {
        const bone_spec *s = &BONE_SPECS[i];
        size_t b = l->bones + i * 0x58;
        put64(d, b, put_name(d, cursor, s->name));
        put16(d, b + 0x20, i);
        put16(d, b + 0x22, (unsigned)s->parent & 0xffff);
        put16(d, b + 0x24, (unsigned)s->smooth & 0xffff);
        put16(d, b + 0x26, (unsigned)s->rigid & 0xffff);
        for (unsigned axis = 0; axis < 3; axis++) {
            putf(d, b + 0x30 + axis * 4, 1.0f);
            putf(d, b + 0x4c + axis * 4, s->t[axis]);
        }
        putf(d, b + 0x3c + 2 * 4, s->rz);
        putf(d, b + 0x3c + 3 * 4, 1.0f);
    }
    for (unsigned k = 0; k < SMOOTH + RIGID; k++)
        put16(d, l->matrices + k * 2, MATRIX_TO_BONE[k]);
}

static void build_shape(uint8_t *d, size_t *cursor, size_t *pool, layout *l, unsigned i)
{
    const shape_spec s = shape_specs(i);
    size_t at = l->shapes + i * 0x60, vertex = take(cursor, 0x58), attrs = take(cursor, s.attr_count * 0x10);
    size_t sizes = take(cursor, 0x10), strides = take(cursor, 0x10), mesh = take(cursor, 0x38);
    size_t vertex_bytes = (size_t)s.vertices * s.stride, index_bytes = (size_t)s.index_count << s.index_format;
    size_t vertex_pool = *pool, index_pool = (vertex_pool + vertex_bytes + 7) & ~(size_t)7;
    *pool = (index_pool + index_bytes + 7) & ~(size_t)7;
    memcpy(d + POOL_AT + vertex_pool, s.vertex_data, vertex_bytes);
    memcpy(d + POOL_AT + index_pool, s.index_data, index_bytes);
    memcpy(d + at, "FSHP", 4);
    put64(d, at + 8, put_name(d, cursor, s.name));
    put64(d, at + 0x10, vertex);
    put64(d, at + 0x18, mesh);
    d[at + 0x5a] = (uint8_t)s.skin;
    d[at + 0x5b] = 1;
    memcpy(d + vertex, "FVTX", 4);
    put64(d, vertex + 8, attrs);
    put64(d, vertex + 0x30, sizes);
    put64(d, vertex + 0x38, strides);
    put32(d, vertex + 0x48, (uint32_t)vertex_pool);
    d[vertex + 0x4c] = (uint8_t)s.attr_count;
    d[vertex + 0x4d] = 1;
    put32(d, vertex + 0x50, s.vertices);
    put32(d, sizes, (uint32_t)vertex_bytes);
    put32(d, strides, s.stride);
    for (unsigned a = 0; a < s.attr_count; a++) {
        put64(d, attrs + a * 0x10, put_name(d, cursor, s.attrs[a].name));
        put32(d, attrs + a * 0x10 + 8, s.attrs[a].format);
        put16(d, attrs + a * 0x10 + 0xc, s.attrs[a].offset);
    }
    put32(d, mesh + 0x20, (uint32_t)index_pool);
    put32(d, mesh + 0x24, OOT_BFRES_TRIANGLE_LIST);
    put32(d, mesh + 0x28, s.index_format);
    put32(d, mesh + 0x2c, s.index_count);
    put32(d, mesh + 0x30, s.first_vertex);
    l->vertex[i] = vertex;
    l->attrs[i] = attrs;
    l->strides[i] = strides;
    l->mesh[i] = mesh;
    l->vertex_pool[i] = vertex_pool;
    l->index_pool[i] = index_pool;
}

/* The inverse of a rotation + translation matrix. */
static void rigid_inverse(const float w[12], float out[12])
{
    for (unsigned r = 0; r < 3; r++) {
        for (unsigned c = 0; c < 3; c++)
            out[r * 4 + c] = w[c * 4 + r];
        out[r * 4 + 3] = -(w[r] * w[3] + w[4 + r] * w[7] + w[8 + r] * w[11]);
    }
}

static void store_inverse(uint8_t *d, const layout *l)
{
    oot_bfres model;
    float world[BONES * 12];
    OK(oot_bfres_open(&model, d, l->size));
    CHECK(!oot_bfres_has_inverse(&model));
    OK(oot_bfres_rest_world(&model, world, BONES));
    for (unsigned k = 0; k < SMOOTH; k++) {
        float inverse[12];
        rigid_inverse(world + MATRIX_TO_BONE[k] * 12, inverse);
        for (unsigned f = 0; f < 12; f++)
            putf(d, l->inverse + k * 48 + f * 4, inverse[f]);
    }
    put64(d, l->skeleton + 0x20, l->inverse);
}

static layout build(uint8_t *d)
{
    layout l;
    size_t cursor = 0, pool = 0;
    memset(&l, 0, sizeof l);
    memset(d, 0, FILE_SIZE);
    for (unsigned i = 0; i < BIG_INDICES; i++)
        big_indices[i] = (uint16_t)(i % 2);
    size_t header = take(&cursor, 0xc0), info = take(&cursor, 0x10), model = take(&cursor, 0x70);
    l.skeleton = take(&cursor, 0x40);
    l.bones = take(&cursor, BONES * 0x58);
    l.matrices = take(&cursor, (SMOOTH + RIGID) * 2);
    l.inverse = take(&cursor, SMOOTH * 48);
    l.shapes = take(&cursor, SHAPES * 0x60);
    l.size = FILE_SIZE;
    memcpy(d + header, "FRES    ", 8);
    put32(d, 8, OOT_BFRES_VERSION);
    d[12] = 0xff;
    d[13] = 0xfe;
    put64(d, 0x28, model);
    put64(d, 0xb0, info);
    put32(d, info, 0x24);
    put32(d, info + 4, FILE_SIZE - POOL_AT);
    put64(d, info + 8, POOL_AT);
    memcpy(d + model, "FMDL", 4);
    put64(d, model + 0x18, l.skeleton);
    put64(d, model + 0x28, l.shapes);
    put16(d, model + 0x6a, SHAPES);
    memcpy(d + l.skeleton, "FSKL", 4);
    put32(d, l.skeleton + 4, 0x1200);
    put64(d, l.skeleton + 0x10, l.bones);
    put64(d, l.skeleton + 0x18, l.matrices);
    put16(d, l.skeleton + 0x38, BONES);
    put16(d, l.skeleton + 0x3a, SMOOTH);
    put16(d, l.skeleton + 0x3c, RIGID);
    build_bones(d, &cursor, &l);
    for (unsigned i = 0; i < SHAPES; i++)
        build_shape(d, &cursor, &pool, &l, i);
    store_inverse(d, &l);
    return l;
}

static int near(float a, float b) { return fabsf(a - b) < 1e-5f; }

static void check_translation(const float *m, float x, float y, float z)
{
    CHECK(near(m[3], x) && near(m[7], y) && near(m[11], z));
}

static void test_hash(void)
{
    CHECK(oot_bfres_fnv1a32("", 0) == 0x811c9dc5u);
    CHECK(oot_bfres_fnv1a32("a", 1) == 0xe40c292cu);
    CHECK(oot_bfres_fnv1a32("foobar", 6) == 0xbf9cf968u);
}

static void test_skeleton(const oot_bfres *m)
{
    oot_bfres_bone bone;
    float world[BONES * 12], palette[(SMOOTH + RIGID) * 12];
    CHECK(m->bone_count == BONES && m->smooth_count == SMOOTH && m->rigid_count == RIGID);
    CHECK(oot_bfres_matrix_count(m) == SMOOTH + RIGID && oot_bfres_has_inverse(m));
    CHECK(oot_bfres_find_bone(m, "Arm_1_L") == 2 && oot_bfres_find_bone(m, "Arm_1") == -1);
    OK(oot_bfres_bone_at(m, 4, &bone));
    CHECK(!strcmp(bone.name, "Spine") && bone.parent == 0 && bone.smooth == 4 && bone.rigid == 6);
    OK(oot_bfres_bone_at(m, 1, &bone));
    CHECK(near(bone.rotation[2], BONE_SPECS[1].rz) && near(bone.scale[0], 1.0f) && near(bone.translation[0], 1.0f));
    REFUSED(oot_bfres_bone_at(m, BONES, &bone));
    CHECK(oot_bfres_smooth_bone(m, 3) == 3 && oot_bfres_rigid_bone(m, 0) == 5 && oot_bfres_rigid_bone(m, 1) == 4);
    CHECK(oot_bfres_smooth_bone(m, SMOOTH) == -1 && oot_bfres_rigid_bone(m, RIGID) == -1);
    CHECK(oot_bfres_matrix_bone(m, SMOOTH + RIGID) == -1);
    REFUSED(oot_bfres_rest_world(m, world, BONES - 1));
    OK(oot_bfres_rest_world(m, world, BONES));
    check_translation(world + 0 * 12, 0, 1, 0);
    check_translation(world + 1 * 12, 1, 1, 0);
    check_translation(world + 2 * 12, 1, 3, 0); /* the clavicle's quarter turn carries the arm */
    check_translation(world + 3 * 12, 1, 4, 0);
    check_translation(world + 5 * 12, 0, 3, 1);
    REFUSED(oot_bfres_palette(m, world, palette, SMOOTH + RIGID - 1));
    OK(oot_bfres_palette(m, world, palette, SMOOTH + RIGID));
    static const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    for (unsigned k = 0; k < SMOOTH; k++)
        for (unsigned f = 0; f < 12; f++)
            CHECK(near(palette[k * 12 + f], identity[f]));
    for (unsigned r = 0; r < RIGID; r++)
        CHECK(!memcmp(palette + (SMOOTH + r) * 12, world + MATRIX_TO_BONE[SMOOTH + r] * 12, 48));
}

static void test_shape_fields(const oot_bfres *m)
{
    oot_bfres_shape s;
    CHECK(m->shape_count == SHAPES);
    REFUSED(oot_bfres_shape_at(m, SHAPES, &s));
    OK(oot_bfres_shape_at(m, 0, &s));
    CHECK(!strcmp(s.name, "ArmShape") && s.skin_count == 2 && s.vertex_count == 6);
    CHECK(s.index_count == 12 && s.index_format == 1 && s.index_size == 2 && s.lod_count == 1);
    CHECK(s.index_hash == oot_bfres_fnv1a32(ARM_INDICES, sizeof ARM_INDICES));
    OK(oot_bfres_shape_at(m, 1, &s));
    CHECK(s.index_size == 4 && s.index_hash == oot_bfres_fnv1a32(FILLER_INDICES, sizeof FILLER_INDICES));
    OK(oot_bfres_shape_at(m, 2, &s));
    CHECK(s.index_count == BIG_INDICES && s.index_hash == oot_bfres_fnv1a32(big_indices, 1024));
    CHECK(s.index_hash != oot_bfres_fnv1a32(big_indices, sizeof big_indices));
    OK(oot_bfres_shape_at(m, 3, &s));
    CHECK(s.index_size == 1 && s.first_vertex == 1 && s.skin_count == 1);
}

static void test_skin(const oot_bfres *m)
{
    oot_bfres_shape s;
    oot_bfres_skin skin;
    OK(oot_bfres_shape_at(m, 0, &s));
    OK(oot_bfres_vertex_skin(&s, 1, &skin));
    CHECK(skin.count == 2 && skin.bone[0] == 3 && skin.bone[1] == 4 && near(skin.weight[0], 128 / 255.0f));
    OK(oot_bfres_vertex_skin(&s, 0, &skin));
    CHECK(skin.count == 1 && skin.bone[0] == 2 && near(skin.weight[0], 1.0f)); /* zero weight left out */
    REFUSED(oot_bfres_vertex_skin(&s, 6, &skin));
    OK(oot_bfres_shape_at(m, 1, &s));
    OK(oot_bfres_vertex_skin(&s, 0, &skin)); /* the filler index 0xff is past skin_count, so not read */
    CHECK(skin.count == 3 && skin.bone[2] == 1);
    OK(oot_bfres_shape_at(m, 2, &s));
    OK(oot_bfres_vertex_skin(&s, 0, &skin));
    CHECK(skin.count == 5 && skin.matrix[4] == 6 && skin.bone[4] == 4 && near(skin.weight[4], 155 / 255.0f));
    OK(oot_bfres_shape_at(m, 3, &s));
    OK(oot_bfres_vertex_skin(&s, 1, &skin));
    CHECK(skin.count == 1 && skin.matrix[0] == 5 && skin.bone[0] == 5 && skin.weight[0] == 1.0f);
}

static void test_marking(const oot_bfres *m)
{
    uint32_t set[OOT_BFRES_BONESET_WORDS(BONES)] = {0};
    uint8_t keep[BIG_INDICES / 3];
    unsigned kept = 99;
    oot_bfres_shape s;
    OK(oot_bfres_boneset_add(m, set, 1));
    OK(oot_bfres_boneset_add_subtree(m, set, 2));
    REFUSED(oot_bfres_boneset_add(m, set, BONES));
    REFUSED(oot_bfres_boneset_add_subtree(m, set, BONES));
    CHECK(oot_bfres_boneset_has(set, 1) && oot_bfres_boneset_has(set, 2) && oot_bfres_boneset_has(set, 3));
    CHECK(!oot_bfres_boneset_has(set, 0) && !oot_bfres_boneset_has(set, 4) && !oot_bfres_boneset_has(set, 5));
    OK(oot_bfres_shape_at(m, 0, &s));
    REFUSED(oot_bfres_mark_triangles(&s, set, keep, 3, &kept));
    OK(oot_bfres_mark_triangles(&s, set, keep, 4, &kept));
    /* 128/255 on the set counts, 127/255 does not; weights on two set bones add up. */
    CHECK(kept == 2 && keep[0] == 1 && keep[1] == 0 && keep[2] == 1 && keep[3] == 0);
    OK(oot_bfres_shape_at(m, 1, &s));
    OK(oot_bfres_mark_triangles(&s, set, keep, 2, &kept));
    CHECK(kept == 1 && keep[0] == 0 && keep[1] == 1);
    OK(oot_bfres_shape_at(m, 3, &s));
    OK(oot_bfres_mark_triangles(&s, set, keep, 1, &kept));
    CHECK(kept == 0 && keep[0] == 0);
    uint32_t rigid_set[OOT_BFRES_BONESET_WORDS(BONES)] = {0};
    OK(oot_bfres_boneset_add(m, rigid_set, 4));
    OK(oot_bfres_boneset_add(m, rigid_set, 5));
    OK(oot_bfres_mark_triangles(&s, rigid_set, keep, 1, &kept));
    CHECK(kept == 1 && keep[0] == 1); /* first_vertex moved the triangle onto vertices 1, 2, 3 */
}

static const char *open_and_world(const uint8_t *d, size_t size)
{
    oot_bfres m;
    float world[BONES * 12];
    const char *error = oot_bfres_open(&m, d, size);
    return error ? error : oot_bfres_rest_world(&m, world, BONES);
}

static const char *open_shape(const uint8_t *d, size_t size, unsigned index)
{
    oot_bfres m;
    oot_bfres_shape s;
    const char *error = oot_bfres_open(&m, d, size);
    return error ? error : oot_bfres_shape_at(&m, index, &s);
}

static void test_refused_headers(const uint8_t *good, const layout *l)
{
    uint8_t d[FILE_SIZE];
    oot_bfres m;
    REFUSED(oot_bfres_open(NULL, good, l->size));
    REFUSED(oot_bfres_open(&m, NULL, l->size));
    REFUSED(oot_bfres_open(&m, good, 0x10));
    memcpy(d, good, FILE_SIZE); d[0] = 'X';
    REFUSED(oot_bfres_open(&m, d, l->size));
    memcpy(d, good, FILE_SIZE); put32(d, 8, 0x000a0300);
    REFUSED(oot_bfres_open(&m, d, l->size));
    memcpy(d, good, FILE_SIZE); d[12] = 0xfe; d[13] = 0xff;
    REFUSED(oot_bfres_open(&m, d, l->size));
    memcpy(d, good, FILE_SIZE); put64(d, 0x28, UINT64_MAX - 4);
    REFUSED(oot_bfres_open(&m, d, l->size));
    memcpy(d, good, FILE_SIZE); put64(d, 0xb0, FILE_SIZE - 1);
    REFUSED(oot_bfres_open(&m, d, l->size));
    memcpy(d, good, FILE_SIZE); put32(d, l->skeleton + 4, 0x0200); /* quaternion rotations */
    REFUSED(oot_bfres_open(&m, d, l->size));
    memcpy(d, good, FILE_SIZE); put64(d, l->skeleton + 0x20, FILE_SIZE - 8);
    REFUSED(oot_bfres_open(&m, d, l->size));
}

static void test_refused_bones(const uint8_t *good, const layout *l)
{
    uint8_t d[FILE_SIZE];
    oot_bfres m;
    memcpy(d, good, FILE_SIZE); put16(d, l->bones + 1 * 0x58 + 0x22, 3); /* parent after the child */
    REFUSED(oot_bfres_open(&m, d, l->size));
    memcpy(d, good, FILE_SIZE); put16(d, l->bones + 2 * 0x58 + 0x20, 7);
    REFUSED(oot_bfres_open(&m, d, l->size));
    memcpy(d, good, FILE_SIZE); put16(d, l->matrices + 2 * 2, 3); /* table disagrees with the bone */
    REFUSED(oot_bfres_open(&m, d, l->size));
    memcpy(d, good, FILE_SIZE); put16(d, l->bones + 5 * 0x58 + 0x26, 0); /* rigid entry in the smooth half */
    REFUSED(oot_bfres_open(&m, d, l->size));
    memcpy(d, good, FILE_SIZE); put16(d, l->bones + 5 * 0x58 + 0x26, 0xffff); /* a palette entry without a bone */
    REFUSED(oot_bfres_open(&m, d, l->size));
    memcpy(d, good, FILE_SIZE); put64(d, l->bones + 3 * 0x58, FILE_SIZE - 1);
    REFUSED(oot_bfres_open(&m, d, l->size));
    memcpy(d, good, FILE_SIZE); putf(d, l->bones + 2 * 0x58 + 0x30, 2.0f); /* a scaled bone with a child */
    REFUSED(open_and_world(d, l->size));
    memcpy(d, good, FILE_SIZE); putf(d, l->bones + 3 * 0x58 + 0x30, 2.0f); /* a scaled leaf is fine */
    OK(open_and_world(d, l->size));
    memcpy(d, good, FILE_SIZE); put32(d, l->bones + 3 * 0x58 + 0x4c, 0x7fc00000); /* NaN */
    REFUSED(open_and_world(d, l->size));
    memcpy(d, good, FILE_SIZE); putf(d, l->bones + 0 * 0x58 + 0x4c, FLT_MAX);
    putf(d, l->bones + 1 * 0x58 + 0x4c, FLT_MAX); /* the sum overflows a float */
    REFUSED(open_and_world(d, l->size));
}

static void test_refused_shapes(const uint8_t *good, const layout *l)
{
    uint8_t d[FILE_SIZE];
    memcpy(d, good, FILE_SIZE); put32(d, l->attrs[0] + 0x10 + 8, 0x1805); /* _w0 as floats */
    REFUSED(open_shape(d, l->size, 0));
    memcpy(d, good, FILE_SIZE); put16(d, l->attrs[0] + 0xc, 4); /* _i0 past the stride */
    REFUSED(open_shape(d, l->size, 0));
    memcpy(d, good, FILE_SIZE); put16(d, l->attrs[0] + 0xe, 1); /* second vertex buffer */
    REFUSED(open_shape(d, l->size, 0));
    memcpy(d, good, FILE_SIZE); d[l->shapes + 0x5a] = 9;
    REFUSED(open_shape(d, l->size, 0));
    memcpy(d, good, FILE_SIZE); d[l->shapes + 0x5a] = 3; /* more influences than components */
    REFUSED(open_shape(d, l->size, 0));
    memcpy(d, good, FILE_SIZE); d[l->shapes + 0x5b] = 0;
    REFUSED(open_shape(d, l->size, 0));
    memcpy(d, good, FILE_SIZE); put32(d, l->strides[0], 0);
    REFUSED(open_shape(d, l->size, 0));
    memcpy(d, good, FILE_SIZE); put32(d, l->vertex[0] + 0x50, 0xffffffffu);
    REFUSED(open_shape(d, l->size, 0));
    memcpy(d, good, FILE_SIZE); put32(d, l->mesh[0] + 0x2c, 0xffffffffu);
    REFUSED(open_shape(d, l->size, 0));
    memcpy(d, good, FILE_SIZE); put32(d, l->mesh[0] + 0x28, 3);
    REFUSED(open_shape(d, l->size, 0));
    memcpy(d, good, FILE_SIZE); put64(d, l->vertex[0] + 8, UINT64_MAX - 8);
    REFUSED(open_shape(d, l->size, 0));
}

static void test_refused_vertices(const uint8_t *good, const layout *l)
{
    uint8_t d[FILE_SIZE], keep[4];
    uint32_t set[OOT_BFRES_BONESET_WORDS(BONES)] = {0};
    oot_bfres m;
    oot_bfres_shape s;
    oot_bfres_skin skin;
    memcpy(d, good, FILE_SIZE); d[POOL_AT + l->vertex_pool[0]] = 200; /* weighted index past the palette */
    OK(oot_bfres_open(&m, d, l->size));
    OK(oot_bfres_shape_at(&m, 0, &s));
    REFUSED(oot_bfres_vertex_skin(&s, 0, &skin));
    memcpy(d, good, FILE_SIZE); d[POOL_AT + l->vertex_pool[0] + 4 * 4 + 1] = 200; /* unweighted: never read as a bone */
    OK(oot_bfres_open(&m, d, l->size));
    OK(oot_bfres_shape_at(&m, 0, &s));
    OK(oot_bfres_vertex_skin(&s, 4, &skin));
    CHECK(skin.count == 1);
    memcpy(d, good, FILE_SIZE); put16(d, POOL_AT + l->index_pool[0], 6); /* index past the vertices */
    OK(oot_bfres_open(&m, d, l->size));
    OK(oot_bfres_shape_at(&m, 0, &s));
    REFUSED(oot_bfres_mark_triangles(&s, set, keep, 4, NULL));
    memcpy(d, good, FILE_SIZE); put32(d, l->mesh[0] + 0x24, 4); /* not a triangle list */
    OK(oot_bfres_open(&m, d, l->size));
    OK(oot_bfres_shape_at(&m, 0, &s));
    REFUSED(oot_bfres_mark_triangles(&s, set, keep, 4, NULL));
    memcpy(d, good, FILE_SIZE); put32(d, l->mesh[0] + 0x2c, 11); /* not a multiple of three */
    OK(oot_bfres_open(&m, d, l->size));
    OK(oot_bfres_shape_at(&m, 0, &s));
    REFUSED(oot_bfres_mark_triangles(&s, set, keep, 3, NULL));
}

/* Runs the whole API over a buffer that may be damaged; it must never read outside it. */
static void exercise_shape(const oot_bfres *m, unsigned index, const uint32_t *set)
{
    oot_bfres_shape s;
    oot_bfres_skin skin;
    if (oot_bfres_shape_at(m, index, &s))
        return;
    for (unsigned v = 0; v < s.vertex_count; v++)
        (void)oot_bfres_vertex_skin(&s, v, &skin);
    size_t triangles = s.index_count / 3;
    uint8_t *keep = malloc(triangles + 1);
    if (!keep) exit(2);
    (void)oot_bfres_mark_triangles(&s, set, keep, triangles, NULL);
    free(keep);
}

static void exercise(const uint8_t *data, size_t size)
{
    uint8_t *copy = malloc(size ? size : 1); /* exact size, so the sanitizer sees any overrun */
    oot_bfres m;
    if (!copy) exit(2);
    memcpy(copy, data, size);
    if (!oot_bfres_open(&m, copy, size)) {
        float *world = malloc((size_t)m.bone_count * 48);
        float *palette = malloc(((size_t)oot_bfres_matrix_count(&m) + 1) * 48);
        uint32_t *set = calloc(OOT_BFRES_BONESET_WORDS(m.bone_count), 4);
        if (!world || !palette || !set) exit(2);
        if (!oot_bfres_rest_world(&m, world, m.bone_count))
            (void)oot_bfres_palette(&m, world, palette, oot_bfres_matrix_count(&m));
        (void)oot_bfres_find_bone(&m, "Arm_1_L");
        (void)oot_bfres_boneset_add_subtree(&m, set, m.bone_count / 2);
        for (unsigned i = 0; i < m.shape_count; i++)
            exercise_shape(&m, i, set);
        free(world); free(palette); free(set);
    }
    free(copy);
    checks++;
}

static void test_damaged(const uint8_t *good, size_t size)
{
    uint8_t d[FILE_SIZE];
    uint32_t random = 0x2f6e2b1u;
    for (size_t cut = 0; cut <= size; cut += cut < POOL_AT ? 1 : 16)
        exercise(good, cut);
    for (unsigned round = 0; round < 40000; round++) {
        memcpy(d, good, size);
        for (unsigned hits = 1 + round % 4; hits; hits--) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            /* Most damage goes to the tables and records; the pool is only bulk data. */
            size_t where = random % (round % 8 ? POOL_AT : size);
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            d[where] = (round & 1) ? (uint8_t)random : (uint8_t)(d[where] ^ (1u << (random % 8)));
        }
        exercise(d, size);
    }
}

int main(void)
{
    static uint8_t file[FILE_SIZE];
    oot_bfres model;
    test_hash();
    layout l = build(file);
    OK(oot_bfres_open(&model, file, l.size));
    test_skeleton(&model);
    test_shape_fields(&model);
    test_skin(&model);
    test_marking(&model);
    test_refused_headers(file, &l);
    test_refused_bones(file, &l);
    test_refused_shapes(file, &l);
    test_refused_vertices(file, &l);
    test_damaged(file, l.size);
    printf("oot_bfres: %lu checks passed\n", checks);
    return 0;
}

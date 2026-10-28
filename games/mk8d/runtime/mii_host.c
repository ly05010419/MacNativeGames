/* Miis without a Mii service. Mario Kart 8 Deluxe (sdk 4.4.0) opens a nn::mii::Database at boot, lists the six default Miis
   for its "Select a Mii", builds 12 random Miis for the title screen's demo race and draws their heads with the SDK's Mii
   renderer linked into main, which reads the console's MiiModel system data. This host answers as a console with no Mii of
   its own: an empty database, the six default Miis every console has, random Miis, and the Mii editor applet cancelled.
   The MiiModel firmware data is not ours to ship: a stand-in of the same layout with every part empty is mounted instead
   (fs_host.c MountSystemData), so heads draw nothing while bodies and favourite colours show.
   What the game needs here was first found by nxnative (MIT, dm/host/hle_mii.c and its MK8D notes); this is this runtime's
   own implementation. Every CharInfo built here is checked with the SDK's own CharInfoRaw::Verify before the game sees it. */
#include "runtime.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MII_NOT_FOUND 0x87Eu        /* nn::mii, module 126, description 4 */
#define MII_CANCELED 0xE7Eu         /* description 7: the player closed the editor; the SDK takes it as success */
#define MII_TEST_MODE 0x1987Eu      /* description 204: importing a database needs the system's test mode */

typedef struct __attribute__((packed)) {
    uint8_t create_id[16]; uint16_t nickname[11];
    uint8_t font_region, favorite_color, gender, height, build, type, region_move;
    uint8_t faceline_type, faceline_color, faceline_wrinkle, faceline_make;
    uint8_t hair_type, hair_color, hair_flip;
    uint8_t eye_type, eye_color, eye_scale, eye_aspect, eye_rotate, eye_x, eye_y;
    uint8_t eyebrow_type, eyebrow_color, eyebrow_scale, eyebrow_aspect, eyebrow_rotate, eyebrow_x, eyebrow_y;
    uint8_t nose_type, nose_scale, nose_y;
    uint8_t mouth_type, mouth_color, mouth_scale, mouth_aspect, mouth_y;
    uint8_t beard_color, beard_type, mustache_type, mustache_scale, mustache_y;
    uint8_t glass_type, glass_color, glass_scale, glass_y;
    uint8_t mole_type, mole_scale, mole_x, mole_y, reserved;
} CharInfo;
_Static_assert(sizeof(CharInfo) == 0x58, "nn::mii::CharInfo is 0x58 bytes");
typedef struct { CharInfo info; uint32_t source; } CharInfoElement;   /* source 1: a default Mii */
_Static_assert(sizeof(CharInfoElement) == 0x5c, "nn::mii::CharInfoElement is 0x5c bytes");

enum { SOURCE_DATABASE = 1, SOURCE_DEFAULT = 2, DEFAULTS = 6, FAVORITE_COLORS = 12 };

/* CharInfoRaw::Verify returns 0 for a valid CharInfo, otherwise the number of the first field out of range. */
static void verify(const CharInfo *c, const char *what) {
    static uint32_t (*check)(const CharInfo *);
    if (!check) check = (uint32_t (*)(const CharInfo *))runtime_symbol("_ZNK2nn3mii6detail11CharInfoRaw6VerifyEv");
    uint32_t r = check(c);
    if (r) { fprintf(stderr, "[mk8d] mii: %s fails CharInfoRaw::Verify (field %#x)\n", what, r); runtime_fail("invalid Mii built by the host"); }
}
/* The system's default Mii: the 3DS-era default parameters with the colours as the Switch's common colour indices (hair 1,
   eyes 8, mouth 19, beard and glasses 8) and the nickname "no name". index >= 0: default Mii <index>, with a fixed create
   ID (UUID version 4) so a game that saved its choice finds the same Mii again; index < 0: a fresh create ID. */
static void build(CharInfo *c, int female, int index) {
    memset(c, 0, sizeof *c);
    if (index >= 0) { uint64_t a = 0x6d6b38642d6d6969ULL + (uint64_t)index, b = 0x64656661756c7400ULL; memcpy(c->create_id, &a, 8); memcpy(c->create_id + 8, &b, 8); }
    else arc4random_buf(c->create_id, sizeof c->create_id);
    c->create_id[6] = (uint8_t)((c->create_id[6] & 0x0f) | 0x40); c->create_id[8] = (uint8_t)((c->create_id[8] & 0x3f) | 0x80);
    static const char name[] = "no name";
    for (size_t i = 0; i < sizeof name - 1; i++) c->nickname[i] = (uint16_t)name[i];
    c->gender = (uint8_t)(female != 0); c->height = 64; c->build = 64;
    c->hair_type = 33; c->hair_color = 1;
    c->eye_type = 2; c->eye_color = 8; c->eye_scale = 4; c->eye_aspect = 3; c->eye_rotate = 4; c->eye_x = 2; c->eye_y = 12;
    c->eyebrow_type = 6; c->eyebrow_color = 1; c->eyebrow_scale = 4; c->eyebrow_aspect = 3; c->eyebrow_rotate = 6; c->eyebrow_x = 2; c->eyebrow_y = 10;
    c->nose_type = 1; c->nose_scale = 4; c->nose_y = 9;
    c->mouth_type = 23; c->mouth_color = 19; c->mouth_scale = 4; c->mouth_aspect = 3; c->mouth_y = 13;
    c->beard_color = 8; c->mustache_scale = 4; c->mustache_y = 10;
    c->glass_color = 8; c->glass_scale = 4; c->glass_y = 10;
    c->mole_scale = 4; c->mole_x = 2; c->mole_y = 20;
}
/* Default Miis 0-2 are male, 3-5 female; favourite colours red, yellow, green, sky blue, purple, white. */
static void build_default(CharInfo *c, int index) {
    build(c, index >= 3, index); c->favorite_color = (uint8_t)(index * 2);
    verify(c, "default Mii");
}

/* nn::mii::Database keeps its service object at +0: the SDK's constructor zeroes it and its destructor releases it only when
   set, so the constructor and destructor run as shipped and the host never sets it. */
static uint32_t db_initialize(void *self) { (void)self; fprintf(stderr, "[mk8d] mii: Database::Initialize (the six default Miis, none of the console's own)\n"); return 0; }
static void db_finalize(void *self) { (void)self; }
static int db_full(const void *self) { (void)self; return 0; }
/* Source flags: 1 the console's database (empty here), 2 the six default Miis. The game asks for the defaults with room for
   six and builds a head for each, so every entry it is given must be valid. */
static uint32_t db_get(void *self, int *count, CharInfoElement *out, int max, int source) {
    (void)self;
    if (!count || max < 0 || (max && !out)) runtime_fail("mii Database::Get: invalid arguments");
    int n = 0;
    for (; (source & SOURCE_DEFAULT) && n < max && n < DEFAULTS; n++) { build_default(&out[n].info, n); out[n].source = 1; }
    *count = n;
    return 0;
}
/* RandomDesc {age, gender, race}: gender 0 male, 1 female, 2 either. Varies what shows without a head (gender, height,
   build, favourite colour); the face is the default one, which draws nothing here. */
static uint32_t db_random(void *self, CharInfo *out, const int32_t *desc) {
    (void)self;
    if (!out || !desc) runtime_fail("mii Database::BuildRandom: invalid arguments");
    int female = desc[1] == 2 ? (int)arc4random_uniform(2) : desc[1] == 1;
    build(out, female, -1);
    out->favorite_color = (uint8_t)arc4random_uniform(FAVORITE_COLORS);
    out->height = (uint8_t)arc4random_uniform(128); out->build = (uint8_t)arc4random_uniform(128);
    verify(out, "random Mii");
    return 0;
}
static uint32_t db_update_latest(const void *self, CharInfo *info, int source) { (void)self; (void)info; (void)source; return MII_NOT_FOUND; }
static uint32_t db_index(const void *self, int *out, const CharInfo *info) { (void)self; (void)info; if (out) *out = -1; return MII_NOT_FOUND; }
static uint32_t db_import(void *self, const void *data, size_t size) {
    (void)self; (void)data; fprintf(stderr, "[mk8d] mii: Database::Import of %zu bytes refused (no system test mode)\n", size); return MII_TEST_MODE;
}
/* The Mii editor (MiiEdit library applet): the player closed it without adding a Mii. */
static uint32_t append_mii(int *index) { (void)index; fprintf(stderr, "[mk8d] mii: AppendMii (no Mii editor here: cancelled)\n"); return MII_CANCELED; }

void *runtime_mii_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn3mii8Database10InitializeEv", db_initialize}, {"_ZN2nn3mii8Database8FinalizeEv", db_finalize},
        {"_ZNK2nn3mii8Database14IsFullDatabaseEv", db_full},
        {"_ZN2nn3mii8Database3GetEPiPNS0_15CharInfoElementEii", db_get},
        {"_ZN2nn3mii8Database11BuildRandomEPNS0_8CharInfoERKNS0_10RandomDescE", db_random},
        {"_ZNK2nn3mii8Database12UpdateLatestEPNS0_8CharInfoEi", db_update_latest},
        {"_ZNK2nn3mii8Database8GetIndexEPiRKNS0_8CharInfoE", db_index},
        {"_ZN2nn3mii8Database6ImportEPKvm", db_import},
        {"_ZN2nn3mii9AppendMiiEPi", append_mii}};
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}

/* ---------- MiiModel system data (0100000000000802) stand-in ----------
   The Mii renderer linked into main reads one texture file (NXTexture{Low,Mid}{Linear,SRGB}.dat) and one shape file
   (Shape{Mid,High}.dat) whole and builds each part of a head from them. The layout, as the code reads it (found by nxnative
   on 4.0.0; the 4.0.1 renderer is the same SDK code):
     texture file: +0x0c the largest decompressed size of each of 11 part kinds, +0x38 their alignments, then from +0x64 one
       0x1c-byte entry per part (327 parts); a texture of size 0 is skipped when a head is built.
     shape file: +0x0c the 12 largest sizes, +0x3c their alignments, from +0x6c one 0x58-byte entry per part (857 parts:
       data offset, inflated size, stored size, zlib parameters, five vertex attribute offsets and sizes, index offset and
       size, bounding box), then the hair (132 x 0x48) and faceline (12 x 0x24) transforms.
   Shapes cannot all be absent: the faceline is drawn without a check, so every shape entry points at one shared mesh, one
   vertex at the origin and the triangle (0, 0, 0): 0x20 bytes of zeros as one stored zlib block. The largest sizes size the
   model's memory pool and the first alignment is that pool's alignment, so each is one page instead of 0. */
enum { TEX_PARTS = 327, SHAPE_PARTS = 857, MESH = 0x20 };
enum { TEX_SIZE = 0x64 + 0x1c * TEX_PARTS, MESH_AT = 0x6c + 0x58 * SHAPE_PARTS + 0x48 * 132 + 0x24 * 12 + 0xc };
_Static_assert(TEX_SIZE == 0x2428 && MESH_AT == 0x14de0, "MiiModel stand-in layout");
static const uint8_t mesh_z[] = {0x78, 0x01, 0x01, MESH, 0, (uint8_t)~MESH, 0xff, [7 ... 7 + MESH - 1] = 0, 0x00, MESH, 0x00, 0x01};   /* stored block; Adler-32 of 32 zeros */
enum { SHAPE_SIZE = MESH_AT + sizeof mesh_z };

static int write_part_file(const char *dir, const char *name, int kinds, size_t size) {
    char path[1280]; if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return 0;
    uint8_t *b = calloc(1, size); if (!b) return 0;
    for (int i = 0; i < kinds; i++) { uint32_t page = 0x1000; memcpy(b + 0xc + 4 * i, &page, 4); memcpy(b + 0xc + 4 * kinds + 4 * i, &page, 4); }
    if (kinds == 12) {
        for (int i = 0; i < SHAPE_PARTS; i++) {
            uint32_t *e = (uint32_t *)(b + 0x6c + 0x58 * i);
            e[0] = MESH_AT; e[1] = MESH; e[2] = sizeof mesh_z;   /* data offset, inflated size, stored size */
            e[9] = 8;                                            /* attribute 0 (the position) is one 8-byte vertex; 1-4 absent */
            e[14] = 0x10; e[15] = 6;                             /* index offset and size: three u16 indices */
        }
        memcpy(b + MESH_AT, mesh_z, sizeof mesh_z);
    }
    FILE *f = fopen(path, "wb"); int ok = f && fwrite(b, 1, size, f) == size;
    if (f && fclose(f)) ok = 0;
    free(b);
    return ok;
}
/* Writes the stand-in into dir once per process; 1 when the files are there. */
int runtime_mii_model_standin(const char *dir) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; static int state;
    pthread_mutex_lock(&lock);
    if (!state) {
        int ok = !mkdir(dir, 0700) || errno == EEXIST;
        static const char *textures[] = {"NXTextureLowLinear.dat", "NXTextureLowSRGB.dat", "NXTextureMidLinear.dat", "NXTextureMidSRGB.dat"};
        static const char *shapes[] = {"ShapeMid.dat", "ShapeHigh.dat"};
        for (int i = 0; ok && i < 4; i++) ok = write_part_file(dir, textures[i], 11, TEX_SIZE);
        for (int i = 0; ok && i < 2; i++) ok = write_part_file(dir, shapes[i], 12, SHAPE_SIZE);
        state = ok ? 1 : -1;
        fprintf(stderr, "[mk8d] mii: MiiModel system data is the host's stand-in in %s%s\n", dir, ok ? " (no textures, one empty mesh: Mii heads draw nothing)" : ": could not be written");
    }
    int ok = state > 0;
    pthread_mutex_unlock(&lock);
    return ok;
}

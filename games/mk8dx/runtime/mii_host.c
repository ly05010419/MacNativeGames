/* Mario Kart 8 Deluxe: the console's Mii database and Mii parts, which the host does not ship.

   The Mii database (nn::mii::Database, its public methods replaced): a console with no Mii of its own (nothing in the
   database: no index, nothing to update) that still has the six default Miis every console has. The game lists those
   at boot with Get(..., 6, SourceFlag Default) to build a head for each, and asks BuildRandom for the title's demo race
   racers. Every CharInfo built here is within nn::mii's CharInfo ranges (CharInfoAccessor::IsValid, which the game
   checks) and has a create id with the UUID version 4 and variant bits; a default Mii's id is fixed, so a choice saved
   in the game finds the same Mii next time. The Mii editor (AppendMii) is not available: cancelled.
   Found in nxnative's notes on this game (games/mk8d/runtime/games/0100152000022000/README.md, "Miis"); this file is
   the Zelda-line runtime's own (games/mk8dx CHANGELOG.md, "Miis").

   The Mii parts (system data MiiModel, 0100000000000802): see mii_model.c. */
#include "mii_host.h"
#ifndef MII_HOST_TEST
#include "runtime.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R_MII_NOT_FOUND ((1u << 9) | 126u)       /* nn::mii: not in the database */
#define R_MII_CANCELLED ((67u << 9) | 126u)     /* nn::mii: the applet was closed without a Mii */

/* The value ranges nn::mii accepts for each CharInfo field, in CharInfo order after the nickname. */
static const struct { uint8_t lo, hi; } mii_range[MII_FIELDS] = {
    {0, 3},   /* font_region */      {0, 11},  /* favorite_color */   {0, 1},   /* gender */
    {0, 127}, /* height */           {0, 127}, /* build */            {0, 1},   /* type */
    {0, 3},   /* region_move */      {0, 11},  /* faceline_type */    {0, 9},   /* faceline_color */
    {0, 11},  /* faceline_wrinkle */ {0, 11},  /* faceline_make */    {0, 131}, /* hair_type */
    {0, 99},  /* hair_color */       {0, 1},   /* hair_flip */        {0, 59},  /* eye_type */
    {0, 99},  /* eye_color */        {0, 7},   /* eye_scale */        {0, 6},   /* eye_aspect */
    {0, 7},   /* eye_rotate */       {0, 12},  /* eye_x */            {0, 18},  /* eye_y */
    {0, 24},  /* eyebrow_type */     {0, 99},  /* eyebrow_color */    {0, 8},   /* eyebrow_scale */
    {0, 6},   /* eyebrow_aspect */   {0, 11},  /* eyebrow_rotate */   {0, 12},  /* eyebrow_x */
    {3, 18},  /* eyebrow_y */        {0, 17},  /* nose_type */        {0, 8},   /* nose_scale */
    {0, 18},  /* nose_y */           {0, 35},  /* mouth_type */       {0, 99},  /* mouth_color */
    {0, 8},   /* mouth_scale */      {0, 6},   /* mouth_aspect */     {0, 18},  /* mouth_y */
    {0, 99},  /* beard_color */      {0, 5},   /* beard_type */       {0, 5},   /* mustache_type */
    {0, 8},   /* mustache_scale */   {0, 16},  /* mustache_y */       {0, 19},  /* glasses_type */
    {0, 99},  /* glasses_color */    {0, 7},   /* glasses_scale */    {0, 20},  /* glasses_y */
    {0, 1},   /* mole_type */        {0, 8},   /* mole_scale */       {0, 16},  /* mole_x */
    {0, 30},  /* mole_y */
};

int mii_charinfo_valid(const MiiCharInfo *c) {
    if (!c || !c->name[0]) return 0;
    for (unsigned i = 0; i < MII_FIELDS; i++) if (c->f[i] < mii_range[i].lo || c->f[i] > mii_range[i].hi) return 0;
    return (c->create_id[6] >> 4) == 4 && (c->create_id[8] & 0xc0) == 0x80 && !c->pad;
}

/* The six default Miis: three men, then three women, in the console's default face (the same parameters as the
   console's built-in defaults' kinds: hair, eyes, brows and mouth per gender), colours as Switch common colours. */
static const uint8_t default_face[2][MII_FIELDS] = {
    /* font fav gen hgt bld typ reg | face col wrk mk | hair col flip | eye col scl asp rot x y | brow col scl asp rot x y |
       nose scl y | mouth col scl asp y | beard col typ | mus scl y | glass col scl y | mole scl x y */
    {0, 0, 0, 64, 64, 0, 0,  0, 0, 0, 0,  33, 1, 0,  2, 8, 4, 3, 4, 2, 12,  6, 1, 4, 3, 6, 2, 10,
     1, 4, 9,  23, 19, 4, 3, 13,  1, 0,  0, 4, 10,  0, 8, 4, 10,  0, 4, 2, 20},
    {0, 0, 1, 64, 64, 0, 0,  0, 0, 0, 0,  12, 1, 0,  4, 8, 4, 3, 3, 2, 12,  0, 1, 4, 3, 6, 2, 10,
     1, 4, 9,  1, 19, 4, 3, 13,  1, 0,  0, 4, 10,  0, 8, 4, 10,  0, 4, 2, 20},
};
static const uint8_t default_favorite[MII_DEFAULTS] = {0, 4, 8, 1, 5, 10};

static void put_name(MiiCharInfo *c, const char *ascii) {
    memset(c->name, 0, sizeof c->name);
    for (unsigned i = 0; ascii[i] && i < 10; i++) c->name[i] = (uint16_t)(unsigned char)ascii[i];
}
static void version4(uint8_t id[16]) { id[6] = (uint8_t)((id[6] & 0x0f) | 0x40); id[8] = (uint8_t)((id[8] & 0x3f) | 0x80); }

void mii_default(MiiCharInfo *out, unsigned index) {
    memset(out, 0, sizeof *out);
    unsigned gender = index >= MII_DEFAULTS / 2;
    memcpy(out->f, default_face[gender], MII_FIELDS);
    out->f[MII_FAVORITE_COLOR] = default_favorite[index % MII_DEFAULTS];
    /* A fixed id per default Mii: "MK8DXdefaultMii" and the index, with the version 4 and variant bits. */
    static const char seed[] = "MK8DXdefaultMii";
    memcpy(out->create_id, seed, 15); out->create_id[15] = (uint8_t)index; version4(out->create_id);
    put_name(out, "no name");
}

void mii_random(MiiCharInfo *out, int gender) {
    memset(out, 0, sizeof *out);
    for (unsigned i = 0; i < MII_FIELDS; i++) out->f[i] = (uint8_t)(mii_range[i].lo + arc4random_uniform(mii_range[i].hi - mii_range[i].lo + 1u));
    /* Random but ordinary: standard font, no special type or moved region, few extras. */
    out->f[MII_FONT_REGION] = 0; out->f[MII_TYPE] = 0; out->f[MII_REGION_MOVE] = 0;
    out->f[MII_GENDER] = (uint8_t)(gender == 0 || gender == 1 ? gender : (int)arc4random_uniform(2));
    out->f[MII_HEIGHT] = (uint8_t)(32 + arc4random_uniform(64)); out->f[MII_BUILD] = (uint8_t)(32 + arc4random_uniform(64));
    if (out->f[MII_GENDER]) { out->f[MII_BEARD_TYPE] = 0; out->f[MII_MUSTACHE_TYPE] = 0; }
    if (arc4random_uniform(4)) out->f[MII_GLASSES_TYPE] = 0;
    if (arc4random_uniform(4)) out->f[MII_MOLE_TYPE] = 0;
    arc4random_buf(out->create_id, sizeof out->create_id); version4(out->create_id);
    put_name(out, "no name");
}

int mii_database_get(MiiCharInfoElement *out, int count, int source_flag) {
    int n = 0;
    /* SourceFlag 1: the console's database (empty here); 2: the default Miis. */
    if (source_flag & 2) for (unsigned i = 0; i < MII_DEFAULTS && n < count; i++, n++) {
        if (out) { mii_default(&out[n].info, i); out[n].source = 1; }
    }
    return n;
}
#ifndef MII_HOST_TEST
/* ---------- nn::mii::Database (the object's first word marks it initialized; the rest is not touched) ---------- */
#define DB_MAGIC 0x4d4b384458444231ULL   /* "MK8DXDB1" */
static void db_construct(uint64_t *self) { if (!self) runtime_fail("mii database object"); *self = 0; }
static void db_destruct(uint64_t *self) { (void)self; }
static uint32_t db_initialize(uint64_t *self) {
    if (!self) runtime_fail("mii database object");
    *self = DB_MAGIC; fprintf(stderr, "[mk8dx] Mii database: none of the console's own Miis, the six default Miis\n");
    return 0;
}
static void db_finalize(uint64_t *self) { if (self) *self = 0; }
static void need_db(const uint64_t *self, const char *what) {
    if (!self || *self != DB_MAGIC) { fprintf(stderr, "[mk8dx] Mii database: %s before Initialize\n", what); runtime_fail("mii database used uninitialized"); }
}
static uint32_t db_get(uint64_t *self, int *out_count, MiiCharInfoElement *out, int count, int source_flag) {
    need_db(self, "Get");
    if (count < 0 || (count && !out)) runtime_fail("mii Get arguments");
    int n = mii_database_get(out, count, source_flag);
    if (out_count) *out_count = n;
    return 0;
}
static void db_build_random(uint64_t *self, MiiCharInfo *out, const int32_t *desc) {
    need_db(self, "BuildRandom");
    if (!out || !desc) runtime_fail("mii BuildRandom arguments");
    mii_random(out, -1);
}
/* The database is empty: no Mii to update, no index for one. */
static uint32_t db_update_latest(const uint64_t *self, MiiCharInfo *info, int source_flag) { need_db(self, "UpdateLatest"); (void)info; (void)source_flag; return R_MII_NOT_FOUND; }
static uint32_t db_get_index(const uint64_t *self, int *out, const MiiCharInfo *info) { need_db(self, "GetIndex"); (void)out; (void)info; return R_MII_NOT_FOUND; }
static unsigned char db_full(const uint64_t *self) { need_db(self, "IsFullDatabase"); return 0; }
static uint32_t append_mii(int *index) { (void)index; fprintf(stderr, "[mk8dx] Mii editor: not available on this host (cancelled)\n"); return R_MII_CANCELLED; }

void *runtime_mii_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn3mii8DatabaseC1Ev", db_construct}, {"_ZN2nn3mii8DatabaseD1Ev", db_destruct},
        {"_ZN2nn3mii8Database10InitializeEv", db_initialize}, {"_ZN2nn3mii8Database8FinalizeEv", db_finalize},
        {"_ZN2nn3mii8Database3GetEPiPNS0_15CharInfoElementEii", db_get},
        {"_ZN2nn3mii8Database11BuildRandomEPNS0_8CharInfoERKNS0_10RandomDescE", db_build_random},
        {"_ZNK2nn3mii8Database12UpdateLatestEPNS0_8CharInfoEi", db_update_latest},
        {"_ZNK2nn3mii8Database8GetIndexEPiRKNS0_8CharInfoE", db_get_index},
        {"_ZNK2nn3mii8Database14IsFullDatabaseEv", db_full},
        {"_ZN2nn3mii9AppendMiiEPi", append_mii},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
#endif   /* MII_HOST_TEST */

/* Mario Kart 8 Deluxe: nn::mii's CharInfo as the host builds it (runtime/mii_host.c), and the MiiModel stand-in
   (runtime/mii_model.c). Kept apart so the tests build them without the rest of the runtime (tests/test_mii.c). */
#pragma once
#include <stdint.h>

#define MII_FIELDS 49
#define MII_DEFAULTS 6
/* Field indices in MiiCharInfo.f (CharInfo order after the nickname). */
enum { MII_FONT_REGION, MII_FAVORITE_COLOR, MII_GENDER, MII_HEIGHT, MII_BUILD, MII_TYPE, MII_REGION_MOVE,
       MII_BEARD_TYPE = 37, MII_MUSTACHE_TYPE = 38, MII_GLASSES_TYPE = 41, MII_MOLE_TYPE = 45 };
typedef struct {
    uint8_t create_id[16];      /* UUID version 4 */
    uint16_t name[11];          /* nickname, UTF-16, ten characters and a terminator */
    uint8_t f[MII_FIELDS];      /* font_region ... mole_y */
    uint8_t pad;
} MiiCharInfo;
_Static_assert(sizeof(MiiCharInfo) == 0x58, "nn::mii::CharInfo");
typedef struct { MiiCharInfo info; uint32_t source; } MiiCharInfoElement;   /* source: 0 database, 1 default */
_Static_assert(sizeof(MiiCharInfoElement) == 0x5c, "nn::mii::CharInfoElement");

int mii_charinfo_valid(const MiiCharInfo *c);           /* every field within nn::mii's ranges, a version 4 create id */
void mii_default(MiiCharInfo *out, unsigned index);     /* the console's default Mii 0..5 */
void mii_random(MiiCharInfo *out, int gender);          /* BuildRandom: gender 0 / 1, or any other value for either */
int mii_database_get(MiiCharInfoElement *out, int count, int source_flag);   /* Database::Get's elements; how many */
void *runtime_mii_override(const char *name);
const char *runtime_mii_model_dir(void);                /* mii_model.c: the stand-in folder, NULL when it cannot be written */

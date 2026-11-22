/* Mario Kart 8 Deluxe host pieces without the game (tests/test_mk8dx.py): the Mii database's CharInfo (runtime/mii_host.c,
   built with MII_HOST_TEST) and the add-on content mount cache (runtime/romfs_work_size.h).
   usage: mii_probe <folder with the add-on content's two files> */
#include "../runtime/mii_host.h"
#include "../runtime/romfs_work_size.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    /* The six default Miis: valid, three men then three women, a fixed id each, all different. */
    MiiCharInfo d[MII_DEFAULTS], again;
    for (unsigned i = 0; i < MII_DEFAULTS; i++) {
        mii_default(&d[i], i);
        CHECK(mii_charinfo_valid(&d[i]), "default Mii %u not valid", i);
        CHECK(d[i].f[MII_GENDER] == (i >= 3), "default Mii %u gender %u", i, d[i].f[MII_GENDER]);
        mii_default(&again, i);
        CHECK(!memcmp(&again, &d[i], sizeof again), "default Mii %u differs between calls (a saved choice must find it again)", i);
        for (unsigned j = 0; j < i; j++) CHECK(memcmp(d[i].create_id, d[j].create_id, 16), "default Miis %u and %u share an id", i, j);
        CHECK(d[i].name[0] == 'n' && d[i].name[7] == 0 && d[i].name[10] == 0, "default Mii %u nickname", i);
    }
    /* BuildRandom: always valid, the asked gender kept, a fresh id each time. */
    MiiCharInfo prev = {0};
    for (int k = 0; k < 2000; k++) {
        MiiCharInfo r; int g = k % 3 == 2 ? -1 : k % 3;
        mii_random(&r, g);
        CHECK(mii_charinfo_valid(&r), "random Mii %d not valid", k);
        if (g >= 0) CHECK(r.f[MII_GENDER] == g, "random Mii %d gender", k);
        CHECK(memcmp(r.create_id, prev.create_id, 16), "random Mii %d repeats an id", k);
        prev = r;
    }
    /* A field out of range or an id without the version bits is refused (the check is not vacuous). */
    MiiCharInfo bad = d[0]; bad.f[MII_FAVORITE_COLOR] = 12; CHECK(!mii_charinfo_valid(&bad), "favorite colour 12 accepted");
    bad = d[0]; bad.f[27] = 2; CHECK(!mii_charinfo_valid(&bad), "eyebrow_y 2 accepted");
    bad = d[0]; bad.create_id[6] &= 0x0f; CHECK(!mii_charinfo_valid(&bad), "create id without version 4 accepted");
    bad = d[0]; bad.name[0] = 0; CHECK(!mii_charinfo_valid(&bad), "empty nickname accepted");
    /* Database::Get: the console's own database is empty; the defaults come with SourceFlag 2, as many as asked for. */
    MiiCharInfoElement e[8]; memset(e, 0xee, sizeof e);
    CHECK(mii_database_get(e, 100, 1) == 0, "the database is not empty");
    CHECK(mii_database_get(e, 8, 2) == 6 && e[5].source == 1 && !memcmp(&e[5].info, &d[5], sizeof d[5]), "Get(.., 6, Default)");
    CHECK(mii_database_get(e, 4, 3) == 4, "Get stops at the count asked for");
    /* The add-on content cache: the two files of the Booster Course Pass's data (contents.dat, version.txt) take 0x88
       bytes of RomFS working memory, the size the game's 0x3130-byte heap leaves room for. */
    uint64_t w = romfs_work_size(argv[1]);
    CHECK(w == 0x88, "add-on content cache %#llx, want 0x88", (unsigned long long)w);
    CHECK(romfs_buckets(1) == 3 && romfs_buckets(5) == 5 && romfs_buckets(6) == 7 && romfs_buckets(19) == 19 && romfs_buckets(20) == 23,
          "RomFS builder bucket counts");
    if (fails) return 1;
    printf("{\"status\":\"pass\",\"probe\":\"mii\",\"scope\":\"default and random Mii CharInfo ranges and ids, Database::Get, add-on content cache 0x88\"}\n");
    return 0;
}

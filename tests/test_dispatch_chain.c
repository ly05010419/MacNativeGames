/* Static CPU test for the shared compute encoder's ordering policy (plan P0).
 *
 * The policy lives in games/oot/third_party/d2r_gfx/gfx/nvn/nvn_dispatch_chain.h and may only
 * remove a barrier it can justify. This test enumerates every policy input combination and
 * compares the function against an independent oracle written here as an explicit rule list, so a
 * change to the policy cannot silently widen what is allowed without updating the oracle too. It
 * then covers the byte-range part: the three hazard directions (write-after-read, read-after-write,
 * write-after-write), disjoint and adjacent ranges, incomplete sets and invalid ranges.
 *
 * No GPU, no game files, no shader seed: only the header above. */
#include <stdio.h>
#include <string.h>

#include "nvn_dispatch_chain.h"

static int failures;

static void check(int cond, const char *what, const nvn_chain_input *in, int got, int want) {
    if (cond) return;
    failures++;
    fprintf(stderr, "FAIL %s: game_barrier=%d indirect=%d prev_indirect=%d concurrent_ok=%d vpool=%d alias=%d -> %d (want %d)\n",
            what, in->game_barrier, in->indirect, in->prev_indirect, in->concurrent_ok, in->vpool_aliases,
            (int)in->alias, got, want);
}

/* Independent oracle: the same contract, stated as a rule table and evaluated in a different order.
 * A barrier must be kept when ANY of: the game asked for ordering, the encoder cannot run an
 * already-ordered pair concurrently, either side's grid is indirect, the run reports virtual
 * aliasing, or the range check did not prove disjointness. Only an all-clear may overlap. */
static nvn_chain_decision oracle(const nvn_chain_input *in) {
    int blockers = 0;
    if (in->game_barrier != 0) blockers++;         /* rule 1 */
    if (in->concurrent_ok == 0) blockers++;        /* rule 2 */
    if (in->indirect != 0) blockers++;             /* rule 3a */
    if (in->prev_indirect != 0) blockers++;        /* rule 3b */
    if (in->vpool_aliases != 0) blockers++;        /* rule 4 */
    if (in->alias != NVN_CHAIN_ALIAS_NONE) blockers++;  /* rule 5 */
    return blockers ? NVN_CHAIN_BARRIER : NVN_CHAIN_CONCURRENT;
}

static void range_case(const char *name, nvn_chain_ranges r, nvn_chain_alias want) {
    nvn_chain_alias got = nvn_chain_alias_of(&r);
    if (got != want) {
        failures++;
        fprintf(stderr, "FAIL range case %s -> %d (want %d)\n", name, (int)got, (int)want);
    }
}

int main(void) {
    nvn_chain_input in;
    int combos = 0;

    /* NULL is never a reason to overlap. */
    if (nvn_chain_decide(NULL) != NVN_CHAIN_BARRIER) {
        failures++;
        fprintf(stderr, "FAIL NULL input must keep the barrier\n");
    }

    /* The one case that overlaps: every gate clear and the ranges proved disjoint. */
    memset(&in, 0, sizeof in);
    in.concurrent_ok = 1;
    if (nvn_chain_decide(&in) != NVN_CHAIN_CONCURRENT) {
        failures++;
        fprintf(stderr, "FAIL all-clear input must overlap\n");
    }

    /* Exhaustive: five booleans and the three-valued range answer. */
    for (int bits = 0; bits < 32; bits++) {
        for (int alias = 0; alias <= 2; alias++) {
            memset(&in, 0, sizeof in);
            in.game_barrier = (bits >> 0) & 1;
            in.indirect = (bits >> 1) & 1;
            in.prev_indirect = (bits >> 2) & 1;
            in.concurrent_ok = (bits >> 3) & 1;
            in.vpool_aliases = (bits >> 4) & 1;
            in.alias = (nvn_chain_alias)alias;
            int got = (int)nvn_chain_decide(&in), want = (int)oracle(&in);
            check(got == want, "enumerated input", &in, got, want);
            combos++;
        }
    }

    /* Named cases, so a failure above also shows the intended reading. */
    struct { int game, ind, pind, conc, vpool, alias, want; const char *name; } cases[] = {
        { 0, 0, 0, 1, 0, NVN_CHAIN_ALIAS_NONE,    NVN_CHAIN_CONCURRENT, "proved disjoint, all clear" },
        { 1, 0, 0, 1, 0, NVN_CHAIN_ALIAS_NONE,    NVN_CHAIN_BARRIER,    "the game asked for ordering" },
        { 0, 0, 0, 0, 0, NVN_CHAIN_ALIAS_NONE,    NVN_CHAIN_BARRIER,    "no concurrent encoding" },
        { 0, 1, 0, 1, 0, NVN_CHAIN_ALIAS_NONE,    NVN_CHAIN_BARRIER,    "this grid is indirect" },
        { 0, 0, 1, 1, 0, NVN_CHAIN_ALIAS_NONE,    NVN_CHAIN_BARRIER,    "previous grid was indirect" },
        { 0, 0, 0, 1, 1, NVN_CHAIN_ALIAS_NONE,    NVN_CHAIN_BARRIER,    "virtual aliasing in this run" },
        { 0, 0, 0, 1, 0, NVN_CHAIN_ALIAS_MAYBE,   NVN_CHAIN_BARRIER,    "ranges may overlap" },
        { 0, 0, 0, 1, 0, NVN_CHAIN_ALIAS_UNKNOWN, NVN_CHAIN_BARRIER,    "no range proof" },
        { 1, 1, 1, 0, 1, NVN_CHAIN_ALIAS_UNKNOWN, NVN_CHAIN_BARRIER,    "everything set" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        memset(&in, 0, sizeof in);
        in.game_barrier = cases[i].game; in.indirect = cases[i].ind;
        in.prev_indirect = cases[i].pind; in.concurrent_ok = cases[i].conc;
        in.vpool_aliases = cases[i].vpool; in.alias = (nvn_chain_alias)cases[i].alias;
        int got = (int)nvn_chain_decide(&in);
        check(got == (int)cases[i].want, cases[i].name, &in, got, cases[i].want);
    }

    /* ---- byte ranges ---- */
    {
        nvn_chain_range w[2] = {{100, 200}, {300, 400}};
        nvn_chain_range r[1] = {{150, 160}};
        nvn_chain_range adjacent[1] = {{200, 300}};
        nvn_chain_range invalid[1] = {{7, 7}};

        /* Nothing on either side: disjoint, so the pair may overlap. */
        range_case("empty sets", (nvn_chain_ranges){.complete = 1}, NVN_CHAIN_ALIAS_NONE);
        /* An incomplete set can never be NONE. */
        range_case("incomplete set",
                   (nvn_chain_ranges){.prev_writes = w, .n_prev_writes = 1, .complete = 0}, NVN_CHAIN_ALIAS_UNKNOWN);
        /* Write-after-read: this dispatch writes where the previous one read. */
        range_case("write-after-read",
                   (nvn_chain_ranges){.prev_reads = r, .n_prev_reads = 1, .cur_writes = r, .n_cur_writes = 1,
                                      .complete = 1}, NVN_CHAIN_ALIAS_MAYBE);
        /* Read-after-write: this dispatch reads what the previous one wrote. */
        range_case("read-after-write",
                   (nvn_chain_ranges){.prev_writes = w, .n_prev_writes = 2, .cur_reads = r, .n_cur_reads = 1,
                                      .complete = 1}, NVN_CHAIN_ALIAS_MAYBE);
        /* Write-after-write on the same bytes. */
        range_case("write-after-write",
                   (nvn_chain_ranges){.prev_writes = w, .n_prev_writes = 1, .cur_writes = w, .n_cur_writes = 1,
                                      .complete = 1}, NVN_CHAIN_ALIAS_MAYBE);
        /* Adjacent half-open ranges share no byte. */
        range_case("adjacent ranges",
                   (nvn_chain_ranges){.prev_writes = w, .n_prev_writes = 2, .cur_reads = adjacent, .n_cur_reads = 1,
                                      .complete = 1}, NVN_CHAIN_ALIAS_NONE);
        /* A range that covers nothing is no hazard. */
        range_case("invalid range covers nothing",
                   (nvn_chain_ranges){.prev_writes = invalid, .n_prev_writes = 1, .cur_reads = invalid, .n_cur_reads = 1,
                                      .complete = 1}, NVN_CHAIN_ALIAS_NONE);
        /* The primitive itself, including a NULL side with a zero count. */
        if (nvn_chain_ranges_intersect(w, 2, adjacent, 1) != 0 ||
            nvn_chain_ranges_intersect(w, 2, r, 1) != 1 ||
            nvn_chain_ranges_intersect(w, 2, NULL, 0) != 0) {
            failures++;
            fprintf(stderr, "FAIL range intersection primitive\n");
        }
    }

    if (failures) {
        fprintf(stderr, "dispatch chain policy: %d failure(s)\n", failures);
        return 1;
    }
    printf("dispatch chain policy: %d enumerated combinations, 9 named policy cases and 8 range cases agree with the oracle\n",
           combos);
    return 0;
}

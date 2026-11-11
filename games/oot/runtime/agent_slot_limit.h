/* An agent's test game never keeps the game slot (user 2026-11-11: "你这样开着游戏 别的session 无法使用 … 必须用代码保证 要写在
   代码里 你的记忆和 文档 不起作用"). Incident that day: an agent's test script was stopped after it had opened Tears of the
   Kingdom, nobody sent "close", and the game sat idle about 9 minutes in the single game slot while other sessions waited.

   A game whose TOTK_SLOT_OWNER is set and is not "user" (claude, codex, claude-oot ...) closes itself the way the input
   file's "close" does (runtime/game_slot.c agent_watch) when
     idle:  no scripted command from the input file and no real user input for TOTK_AGENT_IDLE_S (180) seconds, counted
            from the first presented frame (the warm-up before the title does not count), or
     limit: it has run TOTK_SLOT_EXPECT_S (unset or not a positive number: TOTK_AGENT_LIMIT_S, 600) + TOTK_AGENT_GRACE_S
            (60) seconds, except while the user gave real input in the last 120 s (the user is playing that test window:
            never close under the user, see the 2026-10-15 note in runtime/input.c); it checks again later.
   TOTK_AGENT_IDLE_S=0 / TOTK_AGENT_LIMIT_S=0 turn that check off (a long manual agent session; logged). The user's own
   game (owner user, or no owner at all) is never closed by this.

   Pure functions only, so tests/test_agent_slot_limit.c tests them. games/totk/runtime and games/oot/runtime hold
   identical copies (tests/test_game_slot.py checks). */
#ifndef AGENT_SLOT_LIMIT_H
#define AGENT_SLOT_LIMIT_H
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define AGENT_NEVER (-1.0)   /* a time that has not happened (no first frame, no command, no user input yet) */
enum {
    AGENT_IDLE_DEFAULT_S = 180, AGENT_GRACE_DEFAULT_S = 60, AGENT_LIMIT_DEFAULT_S = 600,
    AGENT_USER_RECENT_S = 120,   /* user input this recent postpones the time limit */
    AGENT_SECONDS_MAX = 7 * 24 * 3600,
};
typedef enum { AGENT_LIMIT_NONE, AGENT_LIMIT_IDLE, AGENT_LIMIT_OVER } AgentLimit;

/* The owner of an agent's game: set, not empty, not "user". */
static inline int agent_owner_is_agent(const char *owner) { return owner && *owner && strcmp(owner, "user") != 0; }

/* Whole seconds 0..AGENT_SECONDS_MAX into *out: 1; unset or empty: 0; anything else (negative, not a number, trailing
   text, too large): -1. */
static inline int agent_seconds_parse(const char *text, long *out) {
    if (!text || !*text) return 0;
    char *end = NULL; errno = 0;
    long v = strtol(text, &end, 10);
    if (errno || end == text || *end || v < 0 || v > AGENT_SECONDS_MAX) return -1;
    *out = v;
    return 1;
}

typedef struct {
    long idle_s;      /* 0: the idle check is off */
    long expect_s;    /* the expected running time the limit uses (TOTK_SLOT_EXPECT_S, else TOTK_AGENT_LIMIT_S / 600) */
    long grace_s;
    long limit_s;     /* expect_s + grace_s; 0: the time limit is off */
    const char *bad;  /* the first variable that was not whole seconds (its default is used), or NULL */
} AgentLimitConfig;

/* The four variables' text (NULL when unset): TOTK_AGENT_IDLE_S, TOTK_AGENT_GRACE_S, TOTK_AGENT_LIMIT_S, TOTK_SLOT_EXPECT_S. */
static inline AgentLimitConfig agent_limit_config(const char *idle, const char *grace, const char *limit, const char *expect) {
    AgentLimitConfig c = { AGENT_IDLE_DEFAULT_S, AGENT_LIMIT_DEFAULT_S, AGENT_GRACE_DEFAULT_S, 0, NULL };
    long v = 0, e = 0;
    int r = agent_seconds_parse(idle, &v);
    if (r > 0) c.idle_s = v; else if (r < 0) c.bad = "TOTK_AGENT_IDLE_S";
    r = agent_seconds_parse(grace, &v);
    if (r > 0) c.grace_s = v; else if (r < 0 && !c.bad) c.bad = "TOTK_AGENT_GRACE_S";
    int limit_off = 0;
    r = agent_seconds_parse(limit, &v);
    if (r > 0) { if (v == 0) limit_off = 1; else c.expect_s = v; }
    else if (r < 0 && !c.bad) c.bad = "TOTK_AGENT_LIMIT_S";
    /* TOTK_SLOT_EXPECT_S: game_slot.py writes "0" when the script gave no expected time; 0 or not a number means none. */
    if (agent_seconds_parse(expect, &e) > 0 && e > 0) c.expect_s = e;
    c.limit_s = limit_off ? 0 : c.expect_s + c.grace_s;
    return c;
}

/* Seconds on one monotonic clock. start: the game took the slot (in this process); shown: its first presented frame
   (AGENT_NEVER before); last_activity: its last scripted command or real user input (AGENT_NEVER: none); last_user: its
   last real user input (AGENT_NEVER: none). idle_s / limit_s: 0 turns that check off. */
static inline AgentLimit agent_limit_decide(double now, double start, double shown, double last_activity, double last_user,
                                            double idle_s, double limit_s) {
    if (idle_s > 0 && shown >= 0) {
        double since = last_activity > shown ? last_activity : shown;
        if (now - since >= idle_s) return AGENT_LIMIT_IDLE;
    }
    if (limit_s > 0 && now - start >= limit_s) {
        int user_playing = last_user >= 0 && now - last_user < AGENT_USER_RECENT_S;
        if (!user_playing) return AGENT_LIMIT_OVER;
    }
    return AGENT_LIMIT_NONE;
}
#endif

/* An agent's test game closes itself when nobody uses it (games/<game>/runtime/agent_slot_limit.h, user 2026-11-11): the
   decision and the configuration, with synthetic times; no game. Built for each runtime's copy with
   -DAGENT_SLOT_LIMIT_H_PATH='"../games/<game>/runtime/agent_slot_limit.h"' (default Tears of the Kingdom's). */
#ifndef AGENT_SLOT_LIMIT_H_PATH
#define AGENT_SLOT_LIMIT_H_PATH "../games/totk/runtime/agent_slot_limit.h"
#endif
#include AGENT_SLOT_LIMIT_H_PATH
#include <stdio.h>

static int failures;
static void expect(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; }
}
#define N AGENT_NEVER

/* The defaults: 180 s idle, 600 + 60 s limit. Times are seconds from the slot taken; the title shows at 100 s. */
static AgentLimit at(double now, double last, double user) { return agent_limit_decide(now, 0, 100, last, user, 180, 660); }

static void idle(void) {
    expect(at(279.9, N, N) == AGENT_LIMIT_NONE, "idle: not before 180 s after the first frame");
    expect(at(280, N, N) == AGENT_LIMIT_IDLE, "idle: at 180 s after the first frame");
    expect(agent_limit_decide(500, 0, N, N, N, 180, 660) == AGENT_LIMIT_NONE, "idle: the warm-up before the first frame does not count");
    expect(at(280, 150, N) == AGENT_LIMIT_NONE, "idle: a command resets it");
    expect(at(329.9, 150, N) == AGENT_LIMIT_NONE, "idle: 180 s from the last command, not before");
    expect(at(330, 150, N) == AGENT_LIMIT_IDLE, "idle: 180 s from the last command");
    expect(at(330, 200, 200) == AGENT_LIMIT_NONE, "idle: user input resets it");
    expect(at(280, 50, N) == AGENT_LIMIT_IDLE, "idle: a command before the first frame does not move the start later than the frame");
    expect(agent_limit_decide(10000, 0, 100, N, N, 0, 0) == AGENT_LIMIT_NONE, "idle 0 and limit 0: never");
    expect(agent_limit_decide(1000, 0, 100, N, N, 0, 660) == AGENT_LIMIT_OVER, "idle 0: off, the limit still applies");
}

static void limit(void) {
    /* A script sending a command every few seconds: only the limit can close it. */
    expect(at(659.9, 650, N) == AGENT_LIMIT_NONE, "limit: not before expect + grace");
    expect(at(660, 655, N) == AGENT_LIMIT_OVER, "limit: at expect + grace");
    expect(agent_limit_decide(10000, 0, 100, 9999, N, 180, 0) == AGENT_LIMIT_NONE, "limit 0: off");
    expect(at(700, 690, 600) == AGENT_LIMIT_NONE, "limit: user input in the last 120 s postpones it");
    expect(at(719.9, 715, 600) == AGENT_LIMIT_NONE, "limit: still postponed just under 120 s after the user's input");
    expect(at(720, 715, 600) == AGENT_LIMIT_OVER, "limit: 120 s after the user's last input it closes");
    expect(agent_limit_decide(900, 0, N, N, N, 180, 660) == AGENT_LIMIT_OVER, "limit: also a game that never showed a frame");
}

static void config(void) {
    AgentLimitConfig c = agent_limit_config(NULL, NULL, NULL, NULL);
    expect(c.idle_s == 180 && c.grace_s == 60 && c.expect_s == 600 && c.limit_s == 660 && !c.bad, "config: defaults");
    c = agent_limit_config(NULL, NULL, NULL, "360");
    expect(c.expect_s == 360 && c.limit_s == 420, "config: TOTK_SLOT_EXPECT_S + grace (measure.py: 360 -> 420)");
    c = agent_limit_config(NULL, NULL, NULL, "0");
    expect(c.limit_s == 660, "config: TOTK_SLOT_EXPECT_S=0 (game_slot.py without an expected time): 600 + 60");
    c = agent_limit_config(NULL, NULL, NULL, "soon");
    expect(c.limit_s == 660, "config: an invalid TOTK_SLOT_EXPECT_S: 600 + 60");
    c = agent_limit_config("3", "5", "20", NULL);
    expect(c.idle_s == 3 && c.grace_s == 5 && c.limit_s == 25 && !c.bad, "config: overrides");
    c = agent_limit_config(NULL, NULL, "20", "100");
    expect(c.limit_s == 160, "config: the expected time wins over TOTK_AGENT_LIMIT_S");
    c = agent_limit_config("0", NULL, "0", "100");
    expect(c.idle_s == 0 && c.limit_s == 0 && !c.bad, "config: 0 turns both checks off");
    c = agent_limit_config(NULL, "0", NULL, "100");
    expect(c.limit_s == 100, "config: grace 0");
    c = agent_limit_config("-5", "x", "1e3", NULL);
    expect(c.idle_s == 180 && c.grace_s == 60 && c.limit_s == 660 && c.bad && !strcmp(c.bad, "TOTK_AGENT_IDLE_S"),
           "config: invalid values keep their defaults and are named");
    c = agent_limit_config("99999999999", NULL, NULL, NULL);
    expect(c.idle_s == 180 && c.bad, "config: too large is invalid");
}

static void owners(void) {
    expect(agent_owner_is_agent("claude") && agent_owner_is_agent("codex-grass") && agent_owner_is_agent("claude-oot"), "owner: agents");
    expect(!agent_owner_is_agent("user"), "owner: the user's game is never closed");
    expect(!agent_owner_is_agent(NULL) && !agent_owner_is_agent(""), "owner: unset is never closed");
}

int main(void) {
    idle();
    limit();
    config();
    owners();
    if (failures) return 1;
    printf("pass: agent game idle and time limits (%s)\n", AGENT_SLOT_LIMIT_H_PATH);
    return 0;
}

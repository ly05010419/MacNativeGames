/* Test the rumble and gyro settings (games/totk/runtime/feedback_settings.h): the words of rumble.txt, rumble-gain.txt
   and gyro.txt, and which of the environment, an agent's test session, the file and the default decides. */
#include "../games/totk/runtime/feedback_settings.h"
#include <math.h>
#include <stdio.h>

static int failures;
static void expect(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; } }

static int on_off(const char *env, const char *owner, const char *file) {
    int from = -1;
    int on = feedback_switch(env, owner, file, &from);
    expect(from >= FEEDBACK_FROM_DEFAULT && from <= FEEDBACK_FROM_BAD_FILE, "what decided is always set");
    return on;
}
static int decided_by(const char *env, const char *owner, const char *file) { int from = -1; feedback_switch(env, owner, file, &from); return from; }
static int gain_of(const char *text, float *gain) { *gain = -1; return feedback_gain_word(text, gain); }

int main(void) {
    /* on / off words, as setting_write saves them ("on\n") or as a person types them */
    expect(feedback_word_on("on") == 1 && feedback_word_on("off") == 0, "on and off");
    expect(feedback_word_on("on\n") == 1 && feedback_word_on("off\n") == 0, "with the newline the runtime writes");
    expect(feedback_word_on("  off \r\n") == 0 && feedback_word_on("\ton\t") == 1, "blanks around the word");
    expect(feedback_word_on("") == FEEDBACK_BAD && feedback_word_on("\n") == FEEDBACK_BAD, "empty");
    expect(feedback_word_on("yes") == FEEDBACK_BAD && feedback_word_on("onn") == FEEDBACK_BAD && feedback_word_on("o") == FEEDBACK_BAD, "other words");
    expect(feedback_word_on("on off") == FEEDBACK_BAD && feedback_word_on("ON") == FEEDBACK_BAD, "two words; capitals, as mouse.txt");
    expect(feedback_word_on(NULL) == FEEDBACK_BAD, "no text");

    /* which decides: TOTK_RUMBLE / TOTK_GYRO ("0" off, anything else on), then an agent's test session (off), then the
       file, then on */
    expect(on_off(NULL, NULL, NULL) == 1, "nothing set: on, as before");
    expect(on_off("0", NULL, "on") == 0 && on_off("1", NULL, "off") == 1, "the environment beats the file");
    expect(on_off("1", "claude", "off") == 1 && on_off("0", "codex", NULL) == 0, "the environment beats an agent session");
    expect(on_off(NULL, "claude", NULL) == 0 && on_off(NULL, "codex", NULL) == 0, "an agent session: off, as before");
    expect(on_off(NULL, "claude", "on") == 0, "an agent session does not read the player's file");
    expect(on_off(NULL, "claude-filecheck", "off") == 0 && on_off(NULL, "user", "on") == 1, "only claude and codex are agents");
    expect(on_off(NULL, NULL, "off\n") == 0 && on_off(NULL, NULL, "on\n") == 1, "the file");
    expect(on_off(NULL, NULL, "maybe") == 1 && decided_by(NULL, NULL, "maybe") == FEEDBACK_FROM_BAD_FILE, "a word it does not know: on, and said so");
    /* what decided, for the log: only the file's own word may be blamed on the file */
    expect(decided_by(NULL, NULL, "off") == FEEDBACK_FROM_FILE && decided_by(NULL, NULL, "on") == FEEDBACK_FROM_FILE, "the file");
    expect(decided_by(NULL, NULL, NULL) == FEEDBACK_FROM_DEFAULT && decided_by(NULL, "user", NULL) == FEEDBACK_FROM_DEFAULT, "no file");
    expect(decided_by("0", NULL, "on") == FEEDBACK_FROM_ENV && decided_by("1", "claude", "off") == FEEDBACK_FROM_ENV, "the environment");
    expect(decided_by(NULL, "claude", "on") == FEEDBACK_FROM_AGENT && decided_by(NULL, "codex", "maybe") == FEEDBACK_FROM_AGENT,
           "an agent session, whatever the file says");

    /* rumble strength: a number in (0, 4], the range TOTK_RUMBLE_GAIN takes */
    float g;
    expect(gain_of("2.5\n", &g) == 1 && fabsf(g - 2.5f) < 1e-6f, "2.5 with a newline");
    expect(gain_of("4", &g) == 1 && g == 4.0f && gain_of(" 1 ", &g) == 1 && g == 1.0f, "4 and 1 with blanks");
    expect(gain_of("0.1", &g) == 1 && fabsf(g - 0.1f) < 1e-6f, "0.1");
    expect(gain_of("0", &g) == 0 && g == -1, "0 is not a strength, and the gain is left alone");
    expect(gain_of("4.01", &g) == 0 && gain_of("-1", &g) == 0, "out of range");
    expect(gain_of("", &g) == 0 && gain_of("abc", &g) == 0 && gain_of("2x", &g) == 0 && gain_of("2 3", &g) == 0, "not one number");
    expect(gain_of("nan", &g) == 0 && gain_of("inf", &g) == 0 && gain_of(NULL, &g) == 0, "nan, inf, no text");
    expect(gain_of("1e400", &g) == 0, "too large for a double");
    expect(gain_of("1e-50", &g) == 0 && g == -1, "above 0 as a double but 0 as a float: silent rumble, so not a strength");
    expect(gain_of("on\n", &g) == 0 && gain_of("1.5 ", &g) == 1 && fabsf(g - 1.5f) < 1e-6f, "a word, and a number with a blank");

    if (failures) { fprintf(stderr, "%d failed\n", failures); return 1; }
    puts("feedback settings: all checks pass");
    return 0;
}

/* The HDR / EDR choice (games/totk/runtime/hdr_auto.h): the hdr.txt words, TOTK_HDR's precedence and what auto means on
   a screen. */
#include "../games/totk/runtime/hdr_auto.h"
#include <stdio.h>

static int failures;
static void expect(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; } }

int main(void) {
    for (int c = HDR_CHOICE_OFF; c <= HDR_CHOICE_AUTO; c++) expect(hdr_choice_parse(NULL, hdr_choice_word(c)) == c, "the word saved for a choice reads back as it");
    expect(!strcmp(hdr_choice_word(HDR_CHOICE_OFF), "off") && !strcmp(hdr_choice_word(HDR_CHOICE_ON), "on") && !strcmp(hdr_choice_word(HDR_CHOICE_AUTO), "auto"), "the on / off words as before, and auto");
    expect(!strcmp(hdr_choice_word(-1), "off") && !strcmp(hdr_choice_word(3), "off"), "an unknown choice saves off");
    expect(hdr_choice_parse("1", NULL) == HDR_CHOICE_ON && hdr_choice_parse("1", "off") == HDR_CHOICE_ON && hdr_choice_parse("1", "auto") == HDR_CHOICE_ON, "TOTK_HDR=1 is on and wins over hdr.txt, as before");
    expect(hdr_choice_parse("auto", NULL) == HDR_CHOICE_AUTO && hdr_choice_parse("auto", "off") == HDR_CHOICE_AUTO && hdr_choice_parse("auto", "on") == HDR_CHOICE_AUTO, "TOTK_HDR=auto is auto and wins over hdr.txt");
    expect(hdr_choice_parse("0", "on") == HDR_CHOICE_OFF && hdr_choice_parse("0", "auto") == HDR_CHOICE_OFF && hdr_choice_parse("on", NULL) == HDR_CHOICE_OFF && hdr_choice_parse("AUTO", "on") == HDR_CHOICE_OFF, "any other TOTK_HDR is off and wins, as before");
    expect(hdr_choice_parse("", "auto") == HDR_CHOICE_AUTO && hdr_choice_parse("", "on") == HDR_CHOICE_ON && hdr_choice_parse("", NULL) == HDR_CHOICE_OFF, "an empty TOTK_HDR leaves it to hdr.txt");
    expect(hdr_choice_parse(NULL, NULL) == HDR_CHOICE_OFF && hdr_choice_parse(NULL, "") == HDR_CHOICE_OFF, "no setting: off");
    expect(hdr_choice_parse(NULL, "1") == HDR_CHOICE_OFF && hdr_choice_parse(NULL, "AUTO") == HDR_CHOICE_OFF && hdr_choice_parse(NULL, "off") == HDR_CHOICE_OFF, "hdr.txt takes its own words only");
    expect(!hdr_choice_effective(HDR_CHOICE_AUTO, 1.0), "auto on an SDR monitor (potential 1): off");
    expect(hdr_choice_effective(HDR_CHOICE_AUTO, 16.0) && hdr_choice_effective(HDR_CHOICE_AUTO, 2.5), "auto on an XDR screen (16) or an HDR monitor: on");
    expect(!hdr_choice_effective(HDR_CHOICE_AUTO, 0.0) && !hdr_choice_effective(HDR_CHOICE_AUTO, NAN) && !hdr_choice_effective(HDR_CHOICE_AUTO, INFINITY), "auto without a usable value: off");
    expect(hdr_choice_effective(HDR_CHOICE_ON, 1.0) && hdr_choice_effective(HDR_CHOICE_ON, NAN) && !hdr_choice_effective(HDR_CHOICE_OFF, 16.0), "on and off do not look at the screen");
    if (failures) return 1;
    puts("pass: HDR / EDR choice words");
    return 0;
}

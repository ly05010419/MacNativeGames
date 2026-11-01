/* Mac output sizes, independent of the game's render targets. Fixed choices bound the drawable in pixels,
   including on Retina displays; preserving the window's shape keeps the compositor from stretching the image. */
#pragma once
#include <math.h>
#include <string.h>

typedef struct { const char *word; unsigned width, height; } OutputResolution;
enum { OUTPUT_RESOLUTION_NATIVE, OUTPUT_RESOLUTION_AUTO, OUTPUT_RESOLUTION_FIRST_FIXED };
static const OutputResolution output_resolutions[] = {
    {"native", 0, 0}, {"auto", 0, 0},
    {"1280x720", 1280, 720}, {"1280x800", 1280, 800}, {"1366x768", 1366, 768},
    {"1440x900", 1440, 900}, {"1600x900", 1600, 900}, {"1680x1050", 1680, 1050},
    {"1920x1080", 1920, 1080}, {"1920x1200", 1920, 1200}, {"2560x1080", 2560, 1080},
    {"2560x1440", 2560, 1440}, {"2560x1600", 2560, 1600}, {"2880x1800", 2880, 1800},
    {"3440x1440", 3440, 1440}, {"3840x1600", 3840, 1600}, {"3840x2160", 3840, 2160},
    {"5120x1440", 5120, 1440}, {"5120x2160", 5120, 2160}, {"5120x2880", 5120, 2880},
    {"6016x3384", 6016, 3384}, {"7680x4320", 7680, 4320},
};
#define OUTPUT_RESOLUTION_COUNT (sizeof output_resolutions / sizeof *output_resolutions)

static int output_resolution_parse(const char *word) {
    size_t n = strcspn(word, "\r\n \t");
    for (unsigned i = 0; i < OUTPUT_RESOLUTION_COUNT; i++)
        if (strlen(output_resolutions[i].word) == n && !strncmp(word, output_resolutions[i].word, n)) return (int)i;
    return OUTPUT_RESOLUTION_NATIVE;
}
static void output_resolution_pixels(int choice, double native_width, double native_height, double *width, double *height) {
    if (choice < 0 || (unsigned)choice >= OUTPUT_RESOLUTION_COUNT || !(*width > 0) || !(*height > 0)) return;
    double target_width = choice == OUTPUT_RESOLUTION_NATIVE ? native_width : output_resolutions[choice].width;
    double target_height = choice == OUTPUT_RESOLUTION_NATIVE ? native_height : output_resolutions[choice].height;
    if (!(target_width > 0) || !(target_height > 0)) return;   /* Auto, or no connected display yet */
    double scale = fmin(target_width / *width, target_height / *height);
    *width = fmax(1, round(*width * scale));
    *height = fmax(1, round(*height * scale));
}

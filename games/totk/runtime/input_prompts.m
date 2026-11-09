/* Live prompt state and platform text drawing. All device selection and binding
   resolution lives in prompt_bindings.h and can be tested without a window/GPU. */
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#include "input_prompts.h"
#include "runtime.h"
#include <pthread.h>
#include <string.h>
#include <stdatomic.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static PromptBindings bindings;
static PromptDevice active = PROMPT_KEYBOARD;
static int style;
static _Atomic int off;
static _Atomic uint64_t generation = 1;
static const char *const style_names[PROMPT_STYLE_OFF + 1] = {"auto", "keyboard", "xbox", "playstation", "switch", "off"};

static void initialize(void) {
    keymap_default(&bindings.keyboard);
    uint8_t modes[CONTROLLER_BUTTONS];
    controller_map_default(bindings.controller, modes);
    char word[32];
    if (runtime_setting_word("prompt-icons.txt", word, sizeof word)) {
        word[strcspn(word, "\r\n")] = 0;
        for (int i = 0; i <= PROMPT_STYLE_OFF; i++)
            if (!strcmp(word, style_names[i])) { style = i; atomic_store(&off, i == PROMPT_STYLE_OFF); break; }
    }
}

void runtime_prompt_keyboard(const KeyMap *map) {
    pthread_once(&once, initialize);
    pthread_mutex_lock(&lock);
    if (memcmp(&bindings.keyboard, map, sizeof *map)) { bindings.keyboard = *map; generation++; }
    pthread_mutex_unlock(&lock);
}
void runtime_prompt_controller(const uint16_t *map) {
    pthread_once(&once, initialize);
    pthread_mutex_lock(&lock);
    if (memcmp(bindings.controller, map, sizeof bindings.controller)) {
        memcpy(bindings.controller, map, sizeof bindings.controller); generation++;
    }
    pthread_mutex_unlock(&lock);
}
void runtime_prompt_mouse_mode(int enabled) {
    pthread_once(&once, initialize);
    pthread_mutex_lock(&lock);
    if (bindings.mouse_enabled != !!enabled) { bindings.mouse_enabled = !!enabled; generation++; }
    pthread_mutex_unlock(&lock);
}
void runtime_prompt_note_input(PromptDevice device) {
    if (device <= PROMPT_AUTO || device >= PROMPT_DEVICE_COUNT) return;
    pthread_once(&once, initialize);
    pthread_mutex_lock(&lock);
    if (active != device) { active = device; if (!style) generation++; }
    pthread_mutex_unlock(&lock);
}
int runtime_prompt_style(void) {
    pthread_once(&once, initialize);
    pthread_mutex_lock(&lock); int value = style; pthread_mutex_unlock(&lock);
    return value;
}
int runtime_prompt_style_set(int value) {
    if (value < 0 || value > PROMPT_STYLE_OFF) return runtime_prompt_style();
    pthread_once(&once, initialize);
    pthread_mutex_lock(&lock);
    if (style != value) { style = value; atomic_store(&off, value == PROMPT_STYLE_OFF); generation++; }
    pthread_mutex_unlock(&lock);
    runtime_setting_save("prompt-icons.txt", style_names[value], "prompt icons");
    return value;
}
int runtime_prompt_off(void) { pthread_once(&once, initialize); return atomic_load_explicit(&off, memory_order_relaxed); }
uint64_t runtime_prompt_snapshot(PromptBindings *out) {
    pthread_once(&once, initialize);
    pthread_mutex_lock(&lock);
    *out = bindings; out->device = style ? (PromptDevice)style : active;
    uint64_t value = generation;
    pthread_mutex_unlock(&lock);
    return value;
}

uint64_t runtime_prompt_generation(void) { return atomic_load(&generation); }

static NSString *key_label(int input) {
    switch (input) {
        case -1: return @"—";
        case 49: return @"␣";
        case 56: case 60: return @"⇧";
        case 59: case 62: return @"⌃";
        case 58: case 61: return @"⌥";
        case 36: return @"↵";
        case 53: return @"Esc";
        case 48: return @"⇥";
        case 51: return @"⌫";
        case 123: return @"←"; case 124: return @"→";
        case 125: return @"↓"; case 126: return @"↑";
        case 128: return @"LMB"; case 129: return @"RMB";
        case 130: return @"MMB"; case 131: return @"M4"; case 132: return @"M5";
        default: {
            const char *label = keymap_input_label(input);
            if (!label) return @"—";
            NSString *s = @(label);
            if ([s hasPrefix:@"Keypad "]) s = [@"K" stringByAppendingString:[s substringFromIndex:7]];
            return s;
        }
    }
}
static NSString *pad_label(PromptDevice device, int input) {
    static NSString *const xbox[] = {@"A", @"B", @"X", @"Y", @"LS", @"RS", @"LB", @"RB", @"LT", @"RT", @"≡", @"▣", @"←", @"↑", @"→", @"↓", @"Pad"};
    static NSString *const ps[] = {@"×", @"○", @"□", @"△", @"L3", @"R3", @"L1", @"R1", @"L2", @"R2", @"≡", @"▣", @"←", @"↑", @"→", @"↓", @"Pad"};
    static NSString *const sw[] = {@"B", @"A", @"Y", @"X", @"L", @"R", @"L", @"R", @"ZL", @"ZR", @"+", @"−", @"←", @"↑", @"→", @"↓", @"Pad"};
    if (input < 0 || input >= 17) return @"—";
    return device == PROMPT_PLAYSTATION ? ps[input] : device == PROMPT_SWITCH ? sw[input] : xbox[input];
}
static void draw_key(CGContextRef ctx, CGRect box, NSString *label, BOOL keyboard) {
    CGContextSetGrayFillColor(ctx, 1, 1);
    CGFloat radius = keyboard ? box.size.height * .13 : box.size.height * .5;
    CGPathRef path = CGPathCreateWithRoundedRect(box, radius, radius, NULL);
    CGContextAddPath(ctx, path); CGContextFillPath(ctx); CGPathRelease(path);
    CTFontRef font = CTFontCreateWithName(CFSTR("Helvetica-Bold"), box.size.height * .58, NULL);
    NSDictionary *attrs = @{(__bridge id)kCTFontAttributeName: (__bridge id)font,
                            (__bridge id)kCTForegroundColorAttributeName: (__bridge id)CGColorGetConstantColor(kCGColorBlack)};
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)[[NSAttributedString alloc] initWithString:label attributes:attrs]);
    CGRect ink = CTLineGetBoundsWithOptions(line, kCTLineBoundsUseGlyphPathBounds);
    CGFloat scale = ink.size.width > 0 && ink.size.height > 0 ?
        fmin(box.size.width * .8 / ink.size.width, box.size.height * .62 / ink.size.height) : 1;
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, CGRectGetMidX(box) - (ink.origin.x + ink.size.width / 2) * scale,
                          CGRectGetMidY(box) - (ink.origin.y + ink.size.height / 2) * scale);
    CGContextScaleCTM(ctx, scale, scale); CGContextSetTextPosition(ctx, 0, 0); CTLineDraw(line, ctx);
    CGContextRestoreGState(ctx); CFRelease(line); CFRelease(font);
}

int runtime_prompt_draw(const PromptBindings *b, unsigned cp, uint8_t *pixels,
                        unsigned width, unsigned height, size_t stride) {
    PromptIcon icon;
    if (!pixels || !width || !height || stride < width || !prompt_resolve(b, cp, &icon)) return 0;
    @autoreleasepool {
        CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
        CGContextRef ctx = CGBitmapContextCreate(pixels, width, height, 8, stride, gray, (CGBitmapInfo)kCGImageAlphaNone);
        CGColorSpaceRelease(gray);
        if (!ctx) return 0;
        CGContextSetGrayFillColor(ctx, 0, 1); CGContextFillRect(ctx, CGRectMake(0, 0, width, height));
        CGFloat margin = fmax(1, fmin(width, height) * .04);
        CGRect box = CGRectInset(CGRectMake(0, 0, width, height), margin, margin);
        for (unsigned i = 0; i < icon.count; i++) {
            CGRect cell = box;
            if (icon.count == 2) { cell.size.width = box.size.width * .47; cell.origin.x += i * box.size.width * .53; }
            if (icon.count == 4) {
                CGFloat w = box.size.width / 3, h = box.size.height / 2;
                cell = CGRectMake(box.origin.x + (i ? i - 1 : 1) * w,
                                  box.origin.y + (i ? 0 : h), w * .92, h * .92);
            }
            draw_key(ctx, cell, icon.device == PROMPT_KEYBOARD ? key_label(icon.input[i]) : pad_label(icon.device, icon.input[i]),
                     icon.device == PROMPT_KEYBOARD);
        }
        CGContextRelease(ctx);
    }
    return 1;
}

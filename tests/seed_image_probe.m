// tests/test_seed_image.py: totk_seed_image.h on parts seed_image.py wrote. Usage: seed_image_probe <folder> <name>...
// Prints "parts <n>", "preloaded <files> <bytes>" (the .variants files copied into memory, seed_image_preload) and per name
// "<name> <length> <sha256>" or "<name> missing"; a name served from the preloaded copy ends with " preloaded", and its
// bytes must be the mapped file's (exit 1 otherwise).
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include "totk_seed_image.h"

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) return 2;
        seed_image img; seed_image_open(&img, @(argv[1]));
        printf("parts %d\n", img.parts);
        seed_image_preloaded *pre = seed_image_preload(&img, ".variants");
        printf("preloaded %zu %zu\n", pre ? pre->files : 0, pre ? pre->len : 0);
        for (int i = 2; i < argc; i++) {
            NSData *d = seed_image_data(&img, @(argv[i])), *p = seed_image_preloaded_data(pre, &img, @(argv[i]));
            if (p && ![p isEqualToData:d]) { printf("%s preloaded bytes differ\n", argv[i]); return 1; }
            if (!d) { printf("%s missing\n", argv[i]); continue; }
            unsigned char md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(d.bytes, (CC_LONG)d.length, md);
            printf("%s %lu ", argv[i], (unsigned long)d.length);
            for (int k = 0; k < CC_SHA256_DIGEST_LENGTH; k++) printf("%02x", md[k]);
            printf("%s\n", p ? " preloaded" : "");
        }
    }
    return 0;
}

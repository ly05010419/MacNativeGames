"""Compile the production defaults helper and exercise real preference domains."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'Foundation requires macOS')
class IOSDeviceDefaults(unittest.TestCase):
    def test_low_memory_defaults_and_saved_choices(self):
        source = r'''
#import <Foundation/Foundation.h>
#include "device_defaults.h"
static int run(NSDictionary *root, BOOL padDevice, uint64_t memory,
               BOOL playerChoice, BOOL expectedDocked, NSString *expectedFPS) {
    NSMutableDictionary *defaults = [NSMutableDictionary dictionary], *pad = [NSMutableDictionary dictionary];
    for (NSDictionary *row in root[@"PreferenceSpecifiers"]) {
        NSString *key = row[@"Key"]; id value = row[@"DefaultValue"];
        if (!key || !value) continue;
        if (padDevice && row[@"DefaultValuePad"]) { value = row[@"DefaultValuePad"]; pad[key] = value; }
        defaults[key] = value;
    }
    NSDictionary *original = [defaults copy];
    totk_ios_botw_device_defaults(defaults, pad, padDevice, memory);
    for (NSString *key in original)
        if (![key isEqual:@"TOTK_DOCKED"] && ![key isEqual:@"TOTK_PHONE_FPS"] &&
            ![defaults[key] isEqual:original[key]]) return 2;
    NSString *domain = [@"mng-ios-defaults-test." stringByAppendingString:NSUUID.UUID.UUIDString];
    NSUserDefaults *d = [[NSUserDefaults alloc] initWithSuiteName:domain];
    @try {
        if (playerChoice) { [d setBool:YES forKey:@"TOTK_DOCKED"]; [d setObject:@"60" forKey:@"TOTK_PHONE_FPS"]; }
        [d registerDefaults:defaults];
        NSDictionary *stored = [d persistentDomainForName:domain] ?: @{};
        for (NSString *key in pad) if (!stored[key]) [d setObject:pad[key] forKey:key];
        if ([d boolForKey:@"TOTK_DOCKED"] != expectedDocked) return 3;
        if (![[d stringForKey:@"TOTK_PHONE_FPS"] isEqual:expectedFPS]) return 4;
        if (padDevice && memory && memory <= (4ULL << 30) && !playerChoice) {
            NSDictionary *saved = [d persistentDomainForName:domain];
            if ([saved[@"TOTK_DOCKED"] boolValue] || ![saved[@"TOTK_PHONE_FPS"] isEqual:@"30"]) return 5;
        }
    } @finally { [d removePersistentDomainForName:domain]; }
    return 0;
}
int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 2) return 1;
    NSDictionary *root = [NSDictionary dictionaryWithContentsOfFile:@(argv[1])];
    if (!root) return 6;
    if (run(root, YES, 4ULL << 30, NO, NO, @"30")) return 10;
    if (run(root, YES, 3ULL << 30, NO, NO, @"30")) return 11;
    if (run(root, YES, 4ULL << 30, YES, YES, @"60")) return 12;
    if (run(root, YES, 8ULL << 30, NO, YES, @"60")) return 13;
    if (run(root, YES, 0, NO, YES, @"60")) return 14;
    if (run(root, NO, 4ULL << 30, NO, NO, @"60")) return 15;
    return 0;
} }
'''
        with tempfile.TemporaryDirectory() as directory:
            path, executable = Path(directory) / 'probe.m', Path(directory) / 'probe'
            path.write_text(source)
            built = subprocess.run(
                ['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                 '-Wno-deprecated-declarations',
                 '-I' + str(ROOT / 'games/totk/ios/Sources'), str(path),
                 '-framework', 'Foundation', '-o', str(executable)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            run = subprocess.run(
                [str(executable), str(ROOT / 'games/totk/ios/Settings.bundle/Root.plist')],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

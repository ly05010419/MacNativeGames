"""A new translation chain keeps the pipeline archive (user 2026-11-15: "为什么要重建？这次必须重建吗？").

The archive folder's identity.json named the chain, and par_open_identity threw the whole folder away when it changed.
From v27 to v28 only 18 of 18,661 translated shaders changed (the other 18,643 byte-identical), so the OOT iPad build
compiled all ~19,700 pipelines from scratch, once per Mac pass, and a player's first start after an update did too. A
pipeline taken from a file is made with MTLPipelineOptionFailOnBinaryArchiveMiss, which misses when its functions
changed, and par_stale writes that one again: the chain is not needed in the identity.

This compiles the two functions straight out of totk_pipeline_archive.h and runs them: a stamp that differs only in the
chain (old stamps carry it) is the same identity; another GPU, OS or archive format is not."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

HEADER = Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r_gfx/gfx/nvn/totk_pipeline_archive.h'


def function(source, name):
    """The text of `static ... name(...) { ... }` (braces balanced)."""
    m = re.search(r'^static [^\n]*\b' + name + r'\(', source, re.M)
    if not m:
        raise AssertionError(f'{name} not found in {HEADER.name}')
    depth, i = 0, source.index('{', m.start())
    for j in range(i, len(source)):
        depth += {'{': 1, '}': -1}.get(source[j], 0)
        if depth == 0:
            return source[m.start():j + 1]
    raise AssertionError(f'{name}: unbalanced braces')


PROGRAM = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#define PAR_SCHEMA 6
#define DMSH_CHAIN "chain-new"
@interface FakeDevice : NSObject
@property(copy) NSString *name;
@end
@implementation FakeDevice
@end
%s
%s
int main(void) { @autoreleasepool {
    FakeDevice *d = [FakeDevice new]; d.name = @"Apple M4";
    NSDictionary *want = par_identity((id<MTLDevice>)d);
    NSString *os = want[@"os"];
    if (want[@"chain"]) { puts("FAIL identity names the chain"); return 1; }
    struct { const char *what; NSDictionary *stamp; BOOL same; } cases[] = {
        {"old stamp, other chain", @{@"chain": @"chain-old", @"gpu": @"Apple M4", @"os": os, @"schema": @6}, YES},
        {"stamp without chain", @{@"gpu": @"Apple M4", @"os": os, @"schema": @6}, YES},
        {"other GPU", @{@"chain": @"chain-new", @"gpu": @"Apple M3", @"os": os, @"schema": @6}, NO},
        {"other OS", @{@"chain": @"chain-new", @"gpu": @"Apple M4", @"os": @"Version 1", @"schema": @6}, NO},
        {"other archive format", @{@"gpu": @"Apple M4", @"os": os, @"schema": @5}, NO},
        {"stamp missing the GPU", @{@"os": os, @"schema": @6}, NO},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
        if (par_same_identity(cases[i].stamp, want) != cases[i].same) { printf("FAIL %%s\n", cases[i].what); return 1; }
    if (par_same_identity(nil, want) || par_same_identity((NSDictionary *)@"x", want)) { puts("FAIL no stamp"); return 1; }
    puts("OK"); return 0;
} }
'''


class ChainKeepsTheArchive(unittest.TestCase):
    def test_only_format_gpu_and_os_throw_the_folder_away(self):
        source = HEADER.read_text()
        program = PROGRAM % (function(source, 'par_identity'), function(source, 'par_same_identity'))
        with tempfile.TemporaryDirectory() as folder:
            src, exe = Path(folder) / 'identity.m', Path(folder) / 'identity'
            src.write_text(program)
            built = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-framework', 'Foundation', '-framework', 'Metal',
                                    str(src), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(ran.stdout.strip(), 'OK', ran.stdout + ran.stderr)

    def test_the_folder_is_opened_with_that_comparison(self):
        body = function(HEADER.read_text(), 'par_open_identity')
        self.assertIn('par_same_identity(stamp, par_identity(dev))', body)
        self.assertNotIn('[stamp isEqual:par_identity(dev)]', body)


if __name__ == '__main__':
    unittest.main()

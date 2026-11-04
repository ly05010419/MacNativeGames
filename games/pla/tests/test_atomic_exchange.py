"""Native regression for an exchange whose spill aliases its reservation granule."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TITLE = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == "darwin", "Apple arm64 native execution")
class ExchangeLowering(unittest.TestCase):
    def test_compiler_loop_lowering_and_native_progress(self):
        with tempfile.TemporaryDirectory(prefix="pla-atomics-") as temporary:
            folder = Path(temporary)
            source = folder / "lower.c"
            source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "arm64_atomics.h"
int main(void) {
    uint32_t original[4] = {0xc85ffd09,0xf90053e9,0xc809fd1f,0x35ffffa9};
    uint32_t code[5], out[4]; uint8_t targets[5];
    assert(arm64_exchange_words(original,out));
    for(unsigned i=0;i<4;i++) printf(".long 0x%08x\n",out[i]);
    memcpy(code,original,sizeof original); code[4]=0xd65f03c0;
    assert(arm64_lower_exchanges(code,5,targets)==1);
    assert(!memcmp(code,out,sizeof out));
    assert(arm64_lower_exchanges(code,5,targets)==0);
    /* A direct branch to the spill must prohibit the transformation. */
    memcpy(code,original,sizeof original); code[4]=0x17fffffd;
    assert(arm64_lower_exchanges(code,5,targets)==0);
    assert(!memcmp(code,original,sizeof original));
    /* Reject a different store value, a different retry, or a different spill. */
    for(unsigned i=1;i<4;i++) {
        memcpy(code,original,sizeof original); code[i]^=1;
        assert(!arm64_exchange_words(code,out));
    }
    assert(arm64_direct_target(0x35ffffa9,3)==0);
    assert(arm64_direct_target(0xd503201f,3)==-1);
}
''')
            generator = folder / "lower"
            subprocess.run(["xcrun", "clang", "-arch", "arm64", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(TITLE / "runtime"), str(source), "-o", str(generator)], check=True)
            words = subprocess.check_output([str(generator)], text=True)
            assembly = folder / "exchange.S"
            assembly.write_text('''.text
.globl _exchange
.p2align 2
_exchange:
    sub sp, sp, #192
    add x8, sp, #168
    mov x9, #42
    str x9, [x8]
''' + words + '''
    ldr x0, [sp, #160]
    ldr x1, [x8]
    orr x0, x0, x1, lsl #32
    orr x0, x0, x9, lsl #48
    add sp, sp, #192
    ret
.globl _assembler_reference
_assembler_reference:
    swpal xzr, x9, [x8]
    ret
''')
            runner = folder / "runner.c"
            runner.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
extern uint64_t exchange(void);
extern void assembler_reference(void);
int main(void) {
    uint32_t word; memcpy(&word,(void *)assembler_reference,4);
    assert(word==0xf8ff8109u); /* independently assembled LSE encoding */
    for(unsigned i=0;i<100000;i++) assert(exchange()==42);
}
''')
            executable = folder / "check"
            subprocess.run(["xcrun", "clang", "-arch", "arm64", "-march=armv8.1-a",
                            str(runner), str(assembly), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()

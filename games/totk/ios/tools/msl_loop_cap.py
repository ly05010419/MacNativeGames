#!/usr/bin/env python3
"""The loop cap of translated shaders (shared/third_party/d2r_gfx/gfx/nvn/msl_loop_cap.h), byte for byte, for the device.

No loop of a translated shader may run more than LOOP_CAP times per invocation: a loop whose count comes from memory the
host bound wrongly ran for minutes in Age of Calamity's first battle, the GPU never finished, and the Mac panicked. A Mac
host caps every MSL it compiles (nvn_mtl.m totk_msl_library with the shared header); the iPhone / iPad load libraries
compiled ahead of time, so build_port_ipa.py caps every port's seed sources before compile_seed.py compiles them.
tests/test_msl_loop_cap.py checks that this and the header's rewrite give the same text.

  python3 games/totk/ios/tools/msl_loop_cap.py <folder of *_b.metal>     (rewrites them in place; prints how many changed)
"""
import sys
from pathlib import Path

LOOP_CAP = 65536


def loop_head(t):
    return t == 'do' or ((t.startswith('for (') or t.startswith('while (')) and t.endswith(')'))


def cap(src):
    if '_mng_loop_' in src:
        return src   # already capped
    lines = src.split('\n')
    out, n, i = [], 0, 0
    while i < len(lines):
        line = lines[i]
        t = line.strip(' \t')
        pad = line[:line.index(t)] if t else ''
        if t and loop_head(t) and i + 1 < len(lines) and lines[i + 1] == pad + '{':
            v = f'_mng_loop_{n}'
            n += 1
            out += [f'{pad}uint {v} = 0u;', line, lines[i + 1], f'{pad}    if (++{v} > {LOOP_CAP}u) break;']
            i += 2
            continue
        out.append(line)
        i += 1
    return '\n'.join(out) if n else src


def cap_folder(folder):
    """Every *_b.metal of folder capped in place (a file is replaced only when it changes); the count changed."""
    changed = 0
    for path in sorted(Path(folder).glob('*_b.metal')):
        text = path.read_text()
        new = cap(text)
        if new != text:
            tmp = path.with_name(path.name + '.cap')
            tmp.write_text(new)
            tmp.replace(path)
            changed += 1
    return changed


if __name__ == '__main__':
    print(cap_folder(sys.argv[1]))

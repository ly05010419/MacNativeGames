# The shader library ABI for the chain's Python tools: the values of dm/gfx/nvn/dmsh_abi.h, the one place the driver segment layout and the binding kinds are written down,
# which the host compiles in. Each "#define DMSH_<NAME> <value>" becomes the attribute <NAME> of this module (dmsh_abi.C2_TEX, dmsh_abi.KIND_CBUF), an int or a str; VALUES holds them all.
# The reader takes only the form the header's top comment describes and stops on any other line, so a value it cannot read never silently goes missing.
# Part of the translation chain (dmsh_chain.py FILES), with the header.
import os, re
from typing import Any

HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'nvn', 'dmsh_abi.h')
_DEFINE = re.compile(r'#define DMSH_([A-Z][A-Z0-9_]*)[ \t]+(0x[0-9A-Fa-f]+|[1-9][0-9]*|0|"[^"\\]*")(?:[ \t]+//.*)?')


def parse(text, path=HEADER):
    """{name without DMSH_: int or str} of a dmsh_abi.h text; ValueError naming the line on anything else"""
    values = {}
    for n, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if not s or s.startswith('//') or s == '#pragma once': continue
        m = _DEFINE.fullmatch(s)
        if not m: raise ValueError(f'{path}:{n}: not a line dmsh_abi.py can read: {s}')
        name, v = m.groups()
        if name in values: raise ValueError(f'{path}:{n}: DMSH_{name} is defined twice')
        values[name] = v[1:-1] if v.startswith('"') else int(v, 0)
    if not values: raise ValueError(f'{path}: no definitions')
    return values


def load(path=HEADER):
    with open(path) as f: return parse(f.read(), path)


VALUES = load()
KINDS = {v for k, v in VALUES.items() if k.startswith('KIND_')}   # every binding kind the build may write
if set(VALUES) & set(globals()): raise ValueError(f'{HEADER}: names this module uses itself: {sorted(set(VALUES) & set(globals()))}')


def __getattr__(name) -> Any:   # dmsh_abi.<NAME> = the value of DMSH_<NAME> (PEP 562: a module attribute found nowhere else is looked up here)
    try: return VALUES[name]
    except KeyError: raise AttributeError(f'{HEADER} defines no DMSH_{name}') from None

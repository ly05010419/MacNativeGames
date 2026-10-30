#!/usr/bin/env python3
# The game name, display version and languages from the NSP's control data (control.nacp, saved in the extraction folder by nx_extract): the iOS app name and version come from them (shared by the packing script and build_ipa.sh),
#   and the packing script checks the game language against the languages the game supports.
# Usage: nacp.py <control.nacp> (prints the game name), nacp.py --version <control.nacp> (prints the display version), nacp.py --languages <control.nacp> (prints the supported language codes);
#   each prints an empty line and exits 1 when none can be read. From Python: import nacp; nacp.name(path), nacp.version(path), nacp.languages(path)
import re, struct, sys, zlib
from pathlib import Path

# The NACP's languages in its own order (the bit order of SupportedLanguageFlag and the order of the title entries): the language code nn::oe::GetDesiredLanguage returns for each, and its name
LANGUAGES = [('en-US', 'American English'), ('en-GB', 'British English'), ('ja', 'Japanese'), ('fr', 'French'), ('de', 'German'), ('es-419', 'Latin American Spanish'), ('es', 'Spanish'), ('it', 'Italian'),
             ('nl', 'Dutch'), ('fr-CA', 'Canadian French'), ('pt', 'Portuguese'), ('ru', 'Russian'), ('ko', 'Korean'), ('zh-Hant', 'Traditional Chinese'), ('zh-Hans', 'Simplified Chinese'), ('pt-BR', 'Brazilian Portuguese')]


def name(path):
    """The title area holds 0x300 bytes per language (name 0x200 + publisher 0x100); take the first non-empty one (0 = American English); a missing file or no sensible name returns an empty string"""
    p = Path(path)
    if not p.is_file(): return ''
    b = p.read_bytes(); t = b[:0x3000]
    try:   # newer SDKs compress the title area: a u16 size + raw deflate, inflating to 32 languages × 0x300 (Mario 1.2.1 does)
        z = zlib.decompress(b[2:2 + struct.unpack_from('<H', b)[0]], -15)
        if len(z) == 0x6000: t = z
    except zlib.error: pass   # not compressed
    for i in range(len(t) // 0x300):
        n = t[i * 0x300:i * 0x300 + 0x200].split(b'\0', 1)[0]
        if not n: continue
        try: n = n.decode('utf-8')
        except UnicodeDecodeError: return ''
        return n if n.isprintable() else ''
    return ''


def version(path):
    """The display version (0x10 bytes at 0x3060, e.g. 1.2.1), cut to what iOS accepts as a bundle version (up to three dot-separated integers); a missing file or no leading number returns an empty string"""
    p = Path(path)
    if not p.is_file(): return ''
    m = re.match(rb'\d+(\.\d+){0,2}', p.read_bytes()[0x3060:0x3070])
    return m.group().decode() if m else ''


def languages(path):
    """The language codes the game supports (the u32 SupportedLanguageFlag at 0x302C, one bit per language in LANGUAGES order; Horizon picks the game language among them), in that order;
    a missing file or no flag returns an empty list"""
    p = Path(path)
    if not p.is_file(): return []
    b = p.read_bytes()
    if len(b) < 0x3030: return []
    f = struct.unpack_from('<I', b, 0x302C)[0]
    return [c for i, (c, _) in enumerate(LANGUAGES) if f >> i & 1]


if __name__ == '__main__':
    a = sys.argv[1:]; f = {'--version': version, '--languages': lambda p: ' '.join(languages(p))}.get(a[0] if a else '', name)
    if f is not name: a = a[1:]
    if len(a) != 1: sys.exit('usage: nacp.py [--version | --languages] <control.nacp>')
    n = f(a[0]); print(n); sys.exit(0 if n else 1)

#!/usr/bin/env python3
"""The Settings pane in the system language (2026-10-29): Root.plist's texts are written "中文 · English"; this writes
Root.strings for en, zh-Hans, zh-Hant and fr into a copy of Settings.bundle, so the Settings app shows one language,
picked from the device's preferred languages (English when none matches). Root.plist itself is left as it is. A text
without the "中文 · English" form, or a French one missing from settings_fr.json, falls back to the English part."""
import json, plistlib, re, sys
from pathlib import Path

CJK = re.compile(r'[　-鿿＀-￯]')
SEP = re.compile(r'\s*·\s*')
# Texts the "中文 · English" rule cannot split: a heading with no English part.
EN_FIXED = {'MetalFX · 超分与抗锯齿': 'MetalFX · Upscaling and anti-aliasing'}

def split(text):
    """(Chinese, English) of a pane text, or None when it has no Chinese part (a language name, a size)."""
    if text in EN_FIXED: return text, EN_FIXED[text]   # the Chinese text keeps its MetalFX
    if not CJK.search(text): return None
    for m in reversed(list(SEP.finditer(text))):
        left, right = text[:m.start()], text[m.end():]
        if right and not CJK.search(right) and CJK.search(left): return left, right
    return None   # Chinese only (繁體中文, 日本語): shown as written

def texts(root):
    for spec in root['PreferenceSpecifiers']:
        for k in ('Title', 'FooterText'):
            if spec.get(k): yield spec[k]
        yield from spec.get('Titles', [])

def tables(root, fr):
    out = {'en': {}, 'zh-Hans': {}, 'zh-Hant': {}, 'fr': {}}
    for t in texts(root):
        s = split(t)
        if not s: continue
        zh, en = s
        out['en'][t] = en; out['zh-Hans'][t] = zh; out['zh-Hant'][t] = zh; out['fr'][t] = fr.get(en, en)
    return out

def localize(bundle):
    bundle = Path(bundle)
    root = plistlib.loads((bundle / 'Root.plist').read_bytes())
    fr = json.loads((Path(__file__).with_name('settings_fr.json')).read_text())
    for lang, table in tables(root, fr).items():
        d = bundle / f'{lang}.lproj'; d.mkdir(exist_ok=True)
        (d / 'Root.strings').write_bytes(plistlib.dumps(table))   # a property-list .strings, read as is by the Settings app

if __name__ == '__main__':
    localize(sys.argv[1])

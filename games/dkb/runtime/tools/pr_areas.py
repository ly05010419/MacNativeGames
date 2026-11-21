#!/usr/bin/env python3
# Area labels for a pull request, from the paths of the files it changes (.github/workflows/pr-areas.yml applies them): which part of nxnative it touches (host, nvn,
# shader chain, …), each game whose profile it changes (game: <title ID>), and "new game" for a profile main does not have yet. The workflow runs this file from main,
# never from the pull request, and gives it only the list of paths.
# Usage: pr_areas.py [--root DIR] < paths   (one path per line; DIR is main's checkout, default this repository)  → one line per label: name, color, description (tab-separated)
import argparse, os, pathlib, re, runpy, sys

AREAS = [   # label, color, description, paths (regular expression, matched from the start)
    ('host', '1d76db', 'The Horizon host: SVCs, services, threads, memory (dm/host)', r'dm/host/'),
    ('nvn', '0e8a16', 'NVN on Metal and its tests (dm/gfx/nvn, dm/gfx/tests)', r'dm/gfx/(nvn|tests)/'),
    ('vulkan', '0e8a16', 'Vulkan on Metal (dm/gfx/vk)', r'dm/gfx/vk/'),
    ('opengl', '0e8a16', "NVIDIA's desktop OpenGL on Metal (dm/gfx/gl)", r'dm/gfx/gl/'),
    ('textures', '0e8a16', 'Texture layouts and formats (dm/gfx/tex)', r'dm/gfx/tex/'),
    ('rewriter', 'fbca04', 'The NSO rewriter and per-build patches', r'dm/tools/(patch_nso|sdk_patches|nso_)|dm/games/[^/]+/patches\.py$'),
    ('packing', 'c5def5', 'Building an IPA or a Mac app: the NSP script, extraction, the shader split, the packers',
     r'scripts/build_ipa_from_nsp\.py$|dm/tools/nx_extract/|dm/ios/tools/build_ipa\.sh$|dm/mac/pack_app\.py$|dm/gfx/tools/shader_split\.py$'),
    ('ios', 'c5def5', 'The iPhone and iPad app (dm/ios)', r'dm/ios/'),
    ('mac', 'c5def5', 'The Mac front end and app (dm/mac, front_mac.m)', r'dm/mac/|dm/host/front_mac\.m$'),
    ('tools', 'bfdadc', 'Tools and scripts (dm/tools, scripts)', r'dm/tools/|scripts/'),
    ('docs', '0075ca', 'Documentation', r'docs/|.*\.md$'),
    ('ci', 'ededed', 'Continuous integration and repository settings (.github)', r'\.github/'),
]
CHAIN = ('shader chain', 'b60205', "The shader translation chain (DMSH_CHAIN): a change rebuilds every game's shaders")   # its files: chain_paths()
GAME_COLOR, NEW_GAME = 'f9d0c4', ('new game', 'd93f0b', 'Adds a game profile that main does not have yet')


def chain_paths(root):   # the files DMSH_CHAIN hashes (main's dmsh_chain.FILES, so the label cannot drift from the chain) and the header it writes
    src = root / 'dm/gfx/tools/dmsh_chain.py'
    if not src.is_file(): return set()
    return {os.path.normpath('dm/gfx/' + f) for f in runpy.run_path(str(src))['FILES']} | {'dm/gfx/nvn/dmsh_chain.h'}


def game_names(root):   # title ID → name, from the games table
    table = (root / 'dm/games/README.md').read_text(encoding='utf-8') if (root / 'dm/games/README.md').is_file() else ''
    return dict((i, n) for n, i in re.findall(r'^\| \[([^\]]+)\]\(([0-9A-F]{16})/README\.md\)', table, re.M))


def labels(paths, root):
    out, names, chain = {}, game_names(root), chain_paths(root)
    for label, color, desc, rx in AREAS:
        if any(re.match(rx, p) for p in paths): out[label] = (color, desc)
    if chain & set(paths): out[CHAIN[0]] = CHAIN[1:]
    for gid in sorted({m.group(1) for p in paths if (m := re.match(r'dm/games/([0-9A-F]{16})/', p))}):
        out[f'game: {gid}'] = (GAME_COLOR, names.get(gid, 'A game profile')[:100])
        if not (root / 'dm/games' / gid / 'game.tsv').is_file(): out[NEW_GAME[0]] = NEW_GAME[1:]
    return out


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='area labels for a pull request from the paths it changes')
    ap.add_argument('--root', default=str(pathlib.Path(__file__).resolve().parents[2]), help="main's checkout (default: this repository)")
    a = ap.parse_args()
    paths = [line.strip() for line in sys.stdin if line.strip()]
    for name, (color, desc) in labels(paths, pathlib.Path(a.root)).items():
        print(f'{name}\t{color}\t{desc}')

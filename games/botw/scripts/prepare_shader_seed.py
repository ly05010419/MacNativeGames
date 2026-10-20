#!/usr/bin/env python3
"""Breath of the Wild: prepare the startup shader seed from the verified local RomFS and the game's shared shader list
(games/botw/assets/shader_list.tsv), as games/totk/scripts/prepare_shader_seed.py does for Tears of the Kingdom (it is
that script with --game botw: Yaz0 packages, the shaders in Pack/, Effect/, Event/ and System/, the control section's
own size in the key; botw.md). The output is game-derived data: it belongs in the work folder, never in the repo.
usage: prepare_shader_seed.py --romfs <RomFS folder> --output <seed folder> --translator <totk_xlate>
       --spirv-cross <spirv-cross> --version botw160 [--modules rewritten/botw160] [--shader-list <list>]
"""
import sys
from pathlib import Path

TOTK_SCRIPTS = next(p for p in (Path(__file__).resolve().parents[2] / 'totk/scripts', Path(__file__).resolve().parents[2] / 'scripts')
                    if (p / 'prepare_shader_seed.py').exists())
sys.path.insert(0, str(TOTK_SCRIPTS))
import prepare_shader_seed  # noqa: E402

if __name__ == '__main__':
    sys.argv[1:1] = ['--game', 'botw']
    prepare_shader_seed.main()

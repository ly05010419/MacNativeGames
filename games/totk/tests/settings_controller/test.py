#!/usr/bin/env python3
"""Build real AppKit settings + gamepad probes. Optional --preview or output PNG; no game data needed."""
import pathlib
import subprocess
import sys
import tempfile

source = pathlib.Path(__file__).with_name('probe.m')
with tempfile.TemporaryDirectory(prefix='totk-settings-controller-') as work:
    binary = pathlib.Path(work) / 'probe'
    subprocess.run(['xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                    '-Wno-deprecated-declarations', '-I' + str(source.parents[2] / 'third_party/d2r_gfx/gfx/nvn/include'),
                    str(source), '-framework', 'Cocoa', '-framework', 'Metal', '-framework', 'CoreVideo',
                    '-framework', 'QuartzCore', '-framework', 'GameController', '-framework', 'IOKit',
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary), work, *sys.argv[1:]], check=True,
                   timeout=None if '--preview' in sys.argv else 30)

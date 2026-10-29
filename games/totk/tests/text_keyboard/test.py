#!/usr/bin/env python3
"""AppKit + real input routing, without game content. Pass --preview or a PNG path after the script."""
import pathlib
import subprocess
import sys
import tempfile

source = pathlib.Path(__file__).with_name('probe.m')
progress_sources = [str(source.parents[2] / 'runtime' / name) for name in
                    ('progress_store.m', 'progress_transport.m', 'progress_sync.m', 'progress_ui.m')]
with tempfile.TemporaryDirectory(prefix='totk-text-keyboard-') as work:
    binary = pathlib.Path(work) / 'probe'
    subprocess.run(['xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                    '-Wno-deprecated-declarations',
                    '-I' + str(source.parents[2] / 'third_party/d2r_gfx/gfx/nvn/include'),
                    str(source), '-framework', 'Cocoa',
                    '-framework', 'QuartzCore', '-framework', 'GameController', '-framework', 'IOKit',
                    '-o', str(binary)], check=True)
    lifecycle = pathlib.Path(work) / 'lifecycle'
    subprocess.run(['xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                    '-Wno-deprecated-declarations',
                    '-I' + str(source.parents[2] / 'third_party/d2r_gfx/gfx/nvn/include'),
                    str(source.with_name('lifecycle.m')), *progress_sources, '-framework', 'Cocoa', '-framework', 'Metal',
                    '-framework', 'CoreVideo', '-framework', 'QuartzCore', '-o', str(lifecycle)], check=True)
    subprocess.run([str(lifecycle)], check=True, timeout=15)
    subprocess.run([str(binary), work, *sys.argv[1:]], check=True,
                   timeout=None if '--preview' in sys.argv else 30)

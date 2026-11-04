#!/usr/bin/env python3
"""Run title tests with synthetic data; native executables stay outside source."""
from pathlib import Path
import subprocess
import sys
import tempfile

TITLE = Path(__file__).resolve().parents[1]
REPO = TITLE.parents[1]

def run(command):
    subprocess.run([str(arg) for arg in command], cwd=REPO, check=True)

def main():
    run([sys.executable, '-B', '-m', 'unittest', 'discover', '-s', TITLE/'tests', '-v'])
    if sys.platform != 'darwin':
        print('Native Cocoa/ARM64 tests require macOS; Python checks completed.')
        return
    with tempfile.TemporaryDirectory(prefix='tomodachi-tests-') as temporary:
        out = Path(temporary)
        common = ['xcrun', 'clang', '-Iruntime']
        for name, sources, flags in [
            ('clock', ['tests/test_tomodachi_clock.c','runtime/time_host.c'], ['-framework','CoreFoundation']),
            ('gesture', ['tests/test_tomodachi_gesture.c','runtime/gesture.c'], []),
            ('inline-keyboard', ['tests/test_tomodachi_inline_keyboard.c','runtime/inline_keyboard.c'], ['-framework','CoreFoundation']),
            ('settings', ['tests/test_tomodachi_settings.m','runtime/settings.m'], ['-fobjc-arc','-DDM_NVN_M3=0','-framework','Cocoa','-framework','GameController']),
        ]:
            binary=out/name
            subprocess.run([*common,*sources,*flags,'-o',str(binary)],cwd=TITLE,check=True)
            run([binary])
            if name=='settings':
                run([binary,'invalid'])
                # Interactive window tests are opt-in on a local desktop.
                if '--ui' in sys.argv:
                    run([binary,'--ui-smoke'])

if __name__=='__main__':
    main()

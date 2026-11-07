"""Pinned, build-time SPIR-V SSA normalizer used by raw-write proof capsules."""
from pathlib import Path
import os
import subprocess

TOOLS = ('https://github.com/KhronosGroup/SPIRV-Tools.git', 'ef96ed763b43b59b33b31b362f09a02b729fa1c9')
HEADERS = ('https://github.com/KhronosGroup/SPIRV-Headers.git', '496543121ce6419f23d6fa5d7194ba66c36212d2')


def build(work, wrapper):
    work = Path(work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    # CMake's compiler probe escapes non-ASCII SDK paths incorrectly. The CLT
    # SDK is sufficient for this CPU-only compiler dependency.
    clt = Path('/Library/Developer/CommandLineTools')
    if clt.is_dir():
        env['DEVELOPER_DIR'] = str(clt)

    def run(cmd):
        subprocess.run([str(v) for v in cmd], check=True, env=env)

    for name, pin in [('tools', TOOLS), ('headers', HEADERS)]:
        folder = work / name
        if not (folder / '.git').exists():
            run(['git', 'init', '-q', folder])
            run(['git', '-C', folder, 'fetch', '-q', '--depth', '1', *pin])
            run(['git', '-C', folder, 'checkout', '-q', 'FETCH_HEAD'])
        actual = subprocess.check_output(['git', '-C', folder, 'rev-parse', 'HEAD'], text=True).strip()
        if actual != pin[1]:
            raise ValueError('SPIR-V proof tool source pin mismatch: ' + name)
    run(['cmake', '-S', work/'tools', '-B', work/'build', '-G', 'Ninja',
         '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_OSX_ARCHITECTURES=arm64',
         '-DSPIRV_SKIP_TESTS=ON', '-DSPIRV_SKIP_EXECUTABLES=ON',
         '-DSPIRV_TOOLS_BUILD_STATIC=ON', '-DBUILD_SHARED_LIBS=OFF',
         '-DSPIRV-Headers_SOURCE_DIR='+str(work/'headers')])
    run(['cmake', '--build', work/'build', '--target', 'SPIRV-Tools-opt', '-j2'])
    obj = work/'ssa-wrapper.o'
    run(['xcrun', 'clang++', '-O2', '-std=c++17', '-arch', 'arm64',
         '-I'+str(work/'tools/include'), '-I'+str(work/'headers/include'), '-c', wrapper, '-o', obj])
    archive = work/'liboot_spirv_tools.a'
    run(['xcrun', 'ar', 'rcs', archive, obj])
    return archive, work/'build/source/opt/libSPIRV-Tools-opt.a', work/'build/source/libSPIRV-Tools.a'

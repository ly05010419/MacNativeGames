#!/usr/bin/env python3
"""Build Skyward Sword HD's shader translator (dmsh_xlate) from the vendored source, into the staged source tree.

    build_translator(source, env)   (scripts/build_ss.py calls it when it stages the source)

The source is third_party/nxnative/{dmsh_xlate,ryujinx}: an unchanged copy of games/mkw/third_party/nxnative (nxnative PR #983,
commit b124b24a), the Switch shader to SPIR-V frontend with nxnative's subset of the Ryujinx shader translator. It is built
framework-dependent for osx-arm64 into <source>/dm/gfx/tools/dmsh_xlate_bin, where the shader chain runs it, with the .NET SDK
of MacNativeGames' tools/deps (scripts/ss_env.py puts it first on PATH and names it in DOTNET_ROOT). NuGet restores the
compiler's packages (pinned in ryujinx/Directory.Packages.props) into the work folder's cache; it needs network access once.

The build is reproducible: no PDB, no git commit in the version stamp (the SDK adds HEAD's when the checkout is a git repository,
so every commit would change the dlls), and the source and artifacts folders mapped to /nxnative and /artifacts (-p:PathMap),
so the dlls do not depend on where the checkout or the work folder are. runtime/gfx/tools/dmsh_chain.lock pins the dlls' fingerprints and the source digest the
translator embeds (--source-sha); dmsh_build.py refuses another build (tool_lock.py). Every bin/ and obj/ goes to the work
folder (--artifacts-path), never beside the sources. A build whose source fingerprint and outputs match its record is reused."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

GAME = Path(__file__).resolve().parents[1]
VENDOR = GAME/'third_party/nxnative'
PROJECT = VENDOR/'dmsh_xlate/dmsh_xlate.csproj'
LOCK = GAME/'runtime/gfx/tools/dmsh_chain.lock'
OUT = Path('dm/gfx/tools/dmsh_xlate_bin')          # in the staged source tree, where dmsh_chain.py looks for it
RECORD = 'dmsh_xlate.build.json'                    # beside the output: what it was built from
BUILD_FLAGS = ('-c', 'Release', '-r', 'osx-arm64', '--self-contained', 'false', '-p:DebugType=none',
               '-p:IncludeSourceRevisionInInformationalVersion=false', '-nologo')


def source_fingerprint(dotnet_version):
    """The vendored source, this script and the SDK version: any change rebuilds."""
    digest = hashlib.sha256(dotnet_version.encode() + b'\0')
    for path in [Path(__file__).resolve(), *sorted(p for p in VENDOR.rglob('*') if p.is_file() and p.parent != VENDOR
                                                    and not {'bin', 'obj', '__pycache__'} & set(p.parts))]:
        digest.update(str(path.relative_to(GAME)).encode() + b'\0' + path.read_bytes() + b'\0')
    return digest.hexdigest()


def outputs(out):
    return {str(p.relative_to(out)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(out.rglob('*')) if p.is_file() and p.name != RECORD}


def lock_source_sha():
    return dict(l.rstrip('\n').split('\t', 1) for l in LOCK.read_text().splitlines(True) if '\t' in l and not l.startswith('#'))['xlate_source']


def embedded_source_sha(exe, env):
    r = subprocess.run([str(exe), '--source-sha'], capture_output=True, text=True, env=env, timeout=60)
    if r.returncode or not r.stdout.strip():
        raise ValueError(f'the shader translator just built does not run ({(r.stderr.strip().splitlines() or ["no output"])[0]}): '
                         'it needs the .NET runtime of MacNativeGames\' tools/deps (DOTNET_ROOT, scripts/ss_env.py)')
    return r.stdout.strip()


def build_translator(source, env, logs=None):
    """<source>/dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate, built or reused; its embedded source digest must be the lock's."""
    dotnet = shutil.which('dotnet', path=env['PATH'])
    if not dotnet:
        raise ValueError('dotnet not found: MacNativeGames\' tools/deps makes the .NET SDK (scripts/ss_env.py puts it on PATH)')
    version = subprocess.run([dotnet, '--version'], capture_output=True, text=True, env=env, check=True).stdout.strip()
    out = Path(source)/OUT
    exe, record = out/'dmsh_xlate', out/RECORD
    fingerprint = source_fingerprint(version)
    try:
        old = json.loads(record.read_text())
        current = old['source_sha256'] == fingerprint and old['outputs'] == outputs(out) and exe.is_file()
    except (OSError, ValueError, KeyError):
        current = False
    if not current:
        work = Path(source).parent
        artifacts = work/'build/dotnet-artifacts/dmsh_xlate'
        shutil.rmtree(out, ignore_errors=True)
        shutil.rmtree(artifacts, ignore_errors=True)
        log = Path(logs or work/'build/logs')/'dmsh_xlate_build.log'
        log.parent.mkdir(parents=True, exist_ok=True)
        path_map = f'-p:PathMap={VENDOR}=/nxnative%2C{artifacts}=/artifacts'   # %2C: MSBuild reads a bare comma as the next property
        cmd = [dotnet, 'build', str(PROJECT), *BUILD_FLAGS, path_map, '--artifacts-path', str(artifacts), '-o', str(out)]
        with open(log, 'w') as f:
            r = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, env=env)
        if r.returncode or not exe.is_file():
            tail = '\n'.join(log.read_text(errors='replace').splitlines()[-20:])
            raise ValueError(f'the shader translator did not build (exit {r.returncode}); {log}:\n{tail}')
        record.write_text(json.dumps({'source_sha256': fingerprint, 'dotnet': version, 'outputs': outputs(out)}, indent=1) + '\n')
    embedded, locked = embedded_source_sha(exe, env), lock_source_sha()
    if embedded != locked:
        raise ValueError(f'the translator built from third_party/nxnative embeds source {embedded}, the lock names {locked}: '
                         'a changed translator source needs tool_lock.py --update-lock, dmsh_chain.py --header and a full shader rebuild')
    return exe

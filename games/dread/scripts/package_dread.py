"""Package Dread's shaders and archive, retaining its existing external game-data layout."""
import json
import os
from pathlib import Path
import plistlib
import shutil
import sys
from dread_bundle import Digests, check_output, signed_bundle, link_data, publish, record_update_source
from dread_pipelines import mac_pipelines, check_archive_lists

GAME = Path(__file__).resolve().parents[1]


def shader_identity(work):
    """Actual translator, SPIRV-Cross, rules and seed inputs; not runtime code."""
    import hashlib
    paths = [GAME/'prebuilt/macos-arm64/totk_xlate', GAME/'scripts/prepare_shader_seed.py',
             GAME/'scripts/shader_list.py', GAME/'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h',
             Path(work)/'build/spirv-cross/spirv-cross']
    values = {str(p.relative_to(GAME)) if p.is_relative_to(GAME) else 'spirv-cross': hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    requests = []
    for line in (GAME/'assets/shader_list.tsv').read_text().splitlines():
        if line.startswith('V\t'):
            _, key, answers = line.split('\t', 2)
            requests.append((key, json.dumps(json.loads(answers), sort_keys=True, separators=(',', ':'))))
    values['variant_requests'] = hashlib.sha256(json.dumps(sorted(set(requests))).encode()).hexdigest()
    return values


def package(builder, py, work, out, logs, inputs):
    check_output(out)
    partial = out.with_name(out.stem+'.partial.app')
    if partial.exists(): raise ValueError(f'partial output exists; inspect it first: {partial}')
    # make compiles the host and launcher into work; it executes neither inside App.
    builder.run(['make', 'prepare', 'runtime', 'app', f'WORK={work}', f'PY={py}', f'APP={partial}'], logs/'app.log')
    profile = work/'build'
    seed = profile/'shader-seed'
    # These are the same lists imported by the runtime; packaging and warm-up share them.
    for name in ('pso_manifest.tsv', 'pso_title.tsv'):
        if (seed/name).is_file(): shutil.copy2(seed/name, profile/name)
        elif name == 'pso_manifest.tsv': raise ValueError('shader seed has no warm-up list')
        elif (profile/name).exists(): (profile/name).unlink()
    # Pipeline-only list updates need no new translations. Resolve their keys
    # against the seed that will actually be bundled, then archive this exact list.
    from prepare_shader_seed import warm_pipelines
    prepared = {p.name.removesuffix('.metal') for p in seed.glob('*_b.metal')}
    rows = (profile/'pso_manifest.tsv').read_text().splitlines()
    rows += ['R\t'+key for key in warm_pipelines(GAME/'assets/shader_list.tsv', prepared)]
    (profile/'pso_manifest.tsv').write_text('\n'.join(dict.fromkeys(rows))+'\n')
    (profile/'shader_map.tsv').write_text('# TOTK control-code-sha256-v1\n')
    env = dict(os.environ, TOTK_SHADER_SEED_DIR=str(seed), TOTK_SHADER_LIB_DIR=str(profile/'shader-libs'),
               TOTK_SHADER_CACHE=str(profile/'pipeline-seed-cache'), TOTK_SHADER_SERVICE='0')
    archive = mac_pipelines(profile/'dread-native', profile, profile/'shader-libs', profile, env)
    digests = Digests(profile/'verified-digests.json')
    shaders = signed_bundle(partial, out, 'ShaderData', {'shader-seed': seed, 'shader-libs': profile/'shader-libs'}, digests, env)
    resources = partial/'Contents/Resources/dread/build'
    resources.mkdir(parents=True)
    for name in ('shader-seed', 'shader-libs'): link_data(resources, name, shaders)
    for name in ('shader_map.tsv', 'pso_manifest.tsv', 'pso_title.tsv'):
        if (profile/name).is_file(): shutil.copy2(profile/name, resources/name)
    shutil.copytree(archive, resources/'pipeline-archive')
    check_archive_lists(resources/'pipeline-archive', resources)
    info = plistlib.loads((partial/'Contents/Info.plist').read_bytes())
    info.update(CFBundleShortVersionString=builder.EXPECTED['display_version'], MNGDreadVersion=builder.VERSION)
    record_update_source(info, work)
    (partial/'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    report = builder.build_info(inputs, work)
    report['shader_identity'] = shader_identity(work)
    report['seed_manifest_sha256'] = digests.sha256(seed/'seed-manifest.json')
    (partial/builder.BUILD_INFO).write_text(json.dumps(report, indent=2)+'\n')
    if not (partial/'Contents/Resources/AppIcon.icns').exists() and (out/'Contents/Resources/AppIcon.icns').is_file():
        shutil.copy2(out/'Contents/Resources/AppIcon.icns', partial/'Contents/Resources/AppIcon.icns')
    builder.run(['codesign', '--force', '--sign', '-', partial], logs/'codesign.log')
    builder.run(['codesign', '--verify', '--strict', partial], logs/'codesign-verify.log')
    publish(partial, out); digests.save()


def blocker(app, work):
    try:
        report = json.loads((Path(app)/'Contents/Resources/build-info.json').read_text())
        if report.get('shader_identity') != shader_identity(work):
            return 'shader translation inputs changed; a full build is required'
        manifest = Path(work)/'build/shader-seed/seed-manifest.json'
        if Digests(Path(work)/'build/verified-digests.json').sha256(manifest) != report.get('seed_manifest_sha256'):
            return 'the shader seed changed or is missing; a full build is required'
        data = Path(app)/'Contents/Helpers/ShaderData-dread.bundle/Contents/Resources'
        expected = json.loads((data/'files.json').read_text())
        digests = Digests(Path(work)/'build/verified-digests.json')
        for rel, sha in expected.items():
            if Path(rel).is_absolute() or '..' in Path(rel).parts or digests.sha256(Path(work)/'build'/rel) != sha:
                return 'prepared shader data changed; a full build is required'
        return None
    except (OSError, ValueError, KeyError):
        return 'this app/work folder has no verified shader metadata; rebuild once'

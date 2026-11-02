"""The build command: the only place in the builder that knows build_app.py's command-line flags (S1.3).

The flags mirror build_app.py's parse_args. The builder always passes --work and -o from the same effective paths the
preflight checked, and never passes --platform (Mac only) or the mod flags (not in this builder yet)."""
from pathlib import Path

from tools.builder.paths import effective_paths


def build_argv(spec, paths, repo_root, python, code_mod=None) -> list[str]:
    """[python, build_app.py, game, -k keys, --work work, -o out, (--update u), (--firmware f | --shared-fonts s),
    (--mod code_mod)]. code_mod (S5.4) is the builder's composed code-mod folder (exefs/ only, so build_app.py merges
    its patches and leaves the RomFS mod folder alone); the caller passes it only for TOTK with an update.
    Raises ValueError('fonts_both') or ValueError('output_not_app') for a build build_app.py would refuse."""
    p = effective_paths(spec, paths)
    if p.firmware is not None and p.shared_fonts is not None:
        raise ValueError('fonts_both')
    if p.out.suffix.lower() != '.app':
        raise ValueError('output_not_app')
    argv = [python, (Path(repo_root) / 'build_app.py').resolve(), p.game, '-k', p.keys, '--work', p.work, '-o', p.out]
    if p.update is not None:
        argv += ['--update', p.update]
    if p.firmware is not None:
        argv += ['--firmware', p.firmware]
    elif p.shared_fonts is not None:
        argv += ['--shared-fonts', p.shared_fonts]
    if code_mod is not None:
        argv += ['--mod', code_mod]
    return [str(a) for a in argv]

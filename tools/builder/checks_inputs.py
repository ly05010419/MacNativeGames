"""The repository, input-file and disk checks (S1.2b). Each rule mirrors build_app.py's own check, with one deliberate
difference: read_kind calls a .xcz card image xcz (not supported), where build_app.py calls it an XCI and fails later.

Every check reads its paths through _paths(ctx), never ctx.paths directly, so all of them, and the build command
(S1.3), see the same work and output folders. Only input_files catches an OSError, and only PermissionError and
IsADirectoryError around open(); everything else propagates to run_checks (K4)."""
import json
from pathlib import Path

from tools.builder.checks import CheckResult, Status
from tools.builder.inputs import BaseKind, Fonts, InputChoice, Status as InputStatus, UpdateKind, evaluate_inputs
from tools.builder.manifests import inspect_versions
from tools.builder.paths import effective_paths

FONTS_MANIFEST = 'games/totk/assets/expected-v1.0.0.json'
FONT_CACHE = 'totk/extracted/firmware/shared-fonts'
# The RomFS caches whose presence spares the space (build_app.py: extracted/<version>/romfs/romfs.bin). BotW's version is
# known only once build_app.py hashes the update, so any BotW cache counts; build_app.py checks the space again itself.
ROMFS_CACHES = {'totk': ('totk/extracted/v0/romfs/romfs.bin',),
                'botw': ('totk/extracted/botw160/romfs/romfs.bin', 'totk/extracted/botw190/romfs/romfs.bin')}
GB = 2 ** 30


def _paths(ctx):
    if ctx.spec is None or ctx.paths is None:
        raise ValueError('the input checks need ctx.spec and ctx.paths')
    return effective_paths(ctx.spec, ctx.paths)


def font_file_names(repo_root):
    """The six '<i>-<name>.bfttf' names from expected-v1.0.0.json, or None if they cannot be read."""
    try:
        data = json.loads((Path(repo_root) / FONTS_MANIFEST).read_bytes().decode('utf-8'))
    except (OSError, UnicodeDecodeError, ValueError):
        return None
    fonts = data.get('shared_fonts_sha256') if isinstance(data, dict) else None
    if not isinstance(fonts, dict) or len(fonts) != 6:
        return None
    return tuple(f'{i}-{name}.bfttf' for i, name in enumerate(fonts))


def read_kind(path):
    """build_app.py's input_kind, except that a .xcz card image is 'xcz' (unsupported) rather than 'xci'."""
    path = Path(path)
    with open(path, 'rb') as f:
        if f.read(4) == b'PFS0':
            return 'nsz' if path.suffix.lower() == '.nsz' else 'nsp'
        f.seek(0x100)
        if f.read(4) == b'HEAD':
            return 'xcz' if path.suffix.lower() == '.xcz' else 'xci'
    return 'unknown'


def cached_fonts(work, names):
    folder = Path(work) / FONT_CACHE
    return names is not None and all((folder / n).is_file() for n in (*names, 'manifest.json'))


def input_choice(spec, paths, names):
    base = BaseKind(read_kind(paths.game))
    if paths.update is None:
        update = UpdateKind.NONE
    else:
        kind = read_kind(paths.update)
        update = UpdateKind.XCI if kind == 'xcz' else UpdateKind(kind)
    fonts = {(False, False): Fonts.NONE, (True, False): Fonts.FIRMWARE, (False, True): Fonts.SHARED,
             (True, True): Fonts.BOTH}[(paths.firmware is not None, paths.shared_fonts is not None)]
    return InputChoice(base, update, fonts, cached_fonts(paths.work, names))


def _block(check_id, message_id, detail):
    return CheckResult(check_id, Status.BLOCK, message_id, detail)


class Repository:
    id, requires = 'repository', ()

    def run(self, ctx):
        _paths(ctx)
        if not (Path(ctx.repo_root) / 'build_app.py').is_file():
            return _block(self.id, 'repository_incomplete', 'build_app.py missing')
        report = inspect_versions(ctx.spec, ctx.repo_root)
        if report.problems or not report.buildable:
            detail = ', '.join(f'{p.path.name}:{p.cause}' for p in report.problems) or 'no buildable version'
            return _block(self.id, 'repository_incomplete', detail)
        if report.unrecognised:
            return CheckResult(self.id, Status.WARN, 'repository_newer_than_builder',
                               ', '.join(p.name for p in report.unrecognised))
        return CheckResult(self.id, Status.OK, 'ok', ', '.join(report.buildable))


class InputFiles:
    id, requires = 'input_files', ()

    def run(self, ctx):
        paths = _paths(ctx)
        for field in ('game', 'keys', 'update'):
            path = getattr(paths, field)
            if path is None:
                continue
            problem = self._file(path, field, f'{field}: {path}')
            if problem:
                return problem
        if paths.firmware is not None and not paths.firmware.is_dir():
            return _block(self.id, 'input_missing', f'firmware: {paths.firmware}')
        if paths.shared_fonts is not None:
            problem = self._fonts(ctx, paths.shared_fonts)
            if problem:
                return problem
        if paths.out.suffix.lower() != '.app':
            return _block(self.id, 'output_not_app', str(paths.out))
        return CheckResult(self.id, Status.OK, 'ok')

    def _fonts(self, ctx, folder):
        names = font_file_names(ctx.repo_root)
        if names is None:
            return _block(self.id, 'font_names_unknown', FONTS_MANIFEST)
        if not folder.is_dir():
            return _block(self.id, 'input_missing', f'shared_fonts: {folder}')
        for name in names:
            problem = self._file(folder / name, f'shared_fonts: {name}', f'shared_fonts: {name}')
            if problem:
                return problem
        return None

    def _file(self, path, label, missing_detail):
        """input_missing, input_unreadable or None for one file that must exist and open."""
        if not path.is_file():
            return _block(self.id, 'input_missing', missing_detail)
        try:
            with open(path, 'rb'):      # opened and closed: nothing is read, no hash computed
                pass
        except (PermissionError, IsADirectoryError) as e:
            return _block(self.id, 'input_unreadable', f'{label}: {e}')
        return None


class InputRules:
    id, requires = 'input_rules', ('input_files',)

    def run(self, ctx):
        paths = _paths(ctx)
        verdict = evaluate_inputs(ctx.spec, input_choice(ctx.spec, paths, font_file_names(ctx.repo_root)))
        if verdict.status is InputStatus.BLOCK:
            return _block(self.id, 'input_rules', ', '.join(verdict.reasons))
        return CheckResult(self.id, Status.OK, 'ok', ', '.join(verdict.notes))


def _work_target(paths):
    """build_app.py's volume target for the work folder (l. 1619, 1647)."""
    return paths.work.parent if paths.work.parent.exists() else paths.game.parent


class DiskSpace:
    id, requires = 'disk_space', ('input_files',)
    REQUIRED_GB = {'totk': (25, 40), 'botw': (22, 22)}    # (without, with) an update

    def run(self, ctx):
        paths = _paths(ctx)
        info = ctx.disk.probe(_work_target(paths))
        if any((paths.work / cache).exists() for cache in ROMFS_CACHES[ctx.spec.id]):
            required = 0
        else:
            required = self.REQUIRED_GB[ctx.spec.id][paths.update is not None]
        free = info.free_bytes / GB
        if free < required:
            return _block(self.id, 'disk_space', f'{free:.1f} GB free on {info.probed}, {required} GB needed')
        return CheckResult(self.id, Status.OK, 'ok', f'{free:.1f} GB free on {info.probed}')


class OutputVolume:
    id, requires = 'output_volume', ('input_files',)

    def run(self, ctx):
        paths = _paths(ctx)
        out_target = paths.out.parent if paths.out.parent.exists() else paths.game.parent    # l. 1648
        work, out = ctx.disk.probe(_work_target(paths)), ctx.disk.probe(out_target)
        if work.device != out.device:
            return CheckResult(self.id, Status.WARN, 'output_other_volume', f'{work.probed} | {out.probed}')
        return CheckResult(self.id, Status.OK, 'ok', str(out.probed))


INPUT_CHECKS = (Repository(), InputFiles(), InputRules(), DiskSpace(), OutputVolume())

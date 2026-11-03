"""Which declared versions of a game this clone can build, checked against its expected-v*.json manifests.

Contract (S1.1, C2):
- A declared manifest is the file its VersionSpec names; it is buildable only if it has no problem.
- Read stage stops at the first failure: missing, unreadable, not_utf8, invalid_json, not_object.
- Content stage reports every defect: display_version, then title_id; each missing_field, wrong_type or mismatch.
- expected-v*.json files that are not declared are listed and never read.
- Nothing on disk makes this raise: every failure becomes a ManifestProblem."""
from dataclasses import dataclass
from fnmatch import fnmatch
import json
from pathlib import Path

from tools.builder.games import GameSpec, VersionSpec

MANIFEST_PATTERN = 'expected-v*.json'
_FIELD_RANK = {None: 0, 'display_version': 1, 'title_id': 2}


@dataclass(frozen=True)
class ManifestProblem:
    path: Path
    field: str | None   # None for the folder and for the read stage
    cause: str
    detail: str = ''


@dataclass(frozen=True)
class VersionReport:
    buildable: tuple[str, ...]           # display versions, in declared order
    problems: tuple[ManifestProblem, ...]
    unrecognised: tuple[Path, ...]


def _list_names(folder):
    """The entry names in folder; patched by tests to simulate listing errors."""
    return [entry.name for entry in folder.iterdir()]


def _read_bytes(path):
    """The bytes of path; patched by tests to simulate read errors."""
    return path.read_bytes()


def inspect_versions(spec: GameSpec, repo_root: Path) -> VersionReport:
    folder = Path(repo_root) / spec.assets_dir
    problems, unrecognised = [], []
    declared = {version.manifest for version in spec.versions}
    try:
        names = _list_names(folder)
    except FileNotFoundError as e:
        problems.append(ManifestProblem(folder, None, 'folder_missing', str(e)))
    except OSError as e:
        problems.append(ManifestProblem(folder, None, 'folder_unreadable', str(e)))
    else:
        unrecognised = sorted(folder / name for name in names
                              if fnmatch(name, MANIFEST_PATTERN) and name not in declared)
    buildable = []
    for version in spec.versions:
        found = _check_manifest(spec, version, folder / version.manifest)
        problems.extend(found)
        if not found:
            buildable.append(version.display)
    problems.sort(key=lambda p: (str(p.path), _FIELD_RANK[p.field]))
    return VersionReport(tuple(buildable), tuple(problems), tuple(unrecognised))


def _check_manifest(spec, version: VersionSpec, path):
    data, problem = _read_manifest(path)
    if problem:
        return [problem]
    return [p for p in (_check_field(path, data, 'display_version', version.display),
                        _check_field(path, data, 'title_id', spec.title_id)) if p]


def _read_manifest(path):
    """(object, None) when the read stage passes, else (None, the first problem)."""
    try:
        raw = _read_bytes(path)
    except FileNotFoundError as e:
        return None, ManifestProblem(path, None, 'missing', str(e))
    except OSError as e:
        return None, ManifestProblem(path, None, 'unreadable', str(e))
    try:
        text = raw.decode('utf-8')
    except UnicodeDecodeError as e:
        return None, ManifestProblem(path, None, 'not_utf8', f'byte {e.start}')
    try:
        data = json.loads(text)
    except json.JSONDecodeError as e:
        return None, ManifestProblem(path, None, 'invalid_json', f'line {e.lineno} column {e.colno}: {e.msg}')
    except ValueError as e:     # valid syntax the parser still refuses: an integer over sys.get_int_max_str_digits()
        return None, ManifestProblem(path, None, 'invalid_json', str(e))
    except RecursionError:
        return None, ManifestProblem(path, None, 'invalid_json', 'nested too deeply')
    if not isinstance(data, dict):
        return None, ManifestProblem(path, None, 'not_object', type(data).__name__)
    return data, None


def _check_field(path, data, field, expected):
    if field not in data:
        return ManifestProblem(path, field, 'missing_field')
    value = data[field]
    if not isinstance(value, str):
        return ManifestProblem(path, field, 'wrong_type', type(value).__name__)
    if value != expected:
        return ManifestProblem(path, field, 'mismatch', f'expected {expected!r}, found {value!r}')
    return None

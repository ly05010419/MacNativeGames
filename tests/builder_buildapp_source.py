"""Test helper: what build_app.py's source declares about games and versions, read with ast (never imported).

It reads the module-level lines the builder's catalog mirrors (build_app.py at origin/main 5281186, lines 49-70):
  GAME = REPO / 'games/totk'                     base folders
  EXPECTED = json.loads((GAME / 'assets/expected-v1.0.0.json').read_text())    manifest loads
  VERSIONS = {'v0': EXPECTED, ...}; VERSIONS['botw160'] = BOTW160               the version registry
  TITLES = {title ID: game}; APPS = {game: (app name, bundle id, data folder)}
A source without these shapes raises SourceLayoutChanged, naming what was not found."""
import ast
import re
from dataclasses import dataclass
from fnmatch import fnmatch

MANIFEST = 'assets/expected-v*.json'


class SourceLayoutChanged(Exception):
    pass


@dataclass(frozen=True)
class BuildAppFacts:
    versions: frozenset      # (game, key, manifest path relative to the repository)
    titles: dict             # title ID -> game
    apps: dict               # game -> (app name, bundle id, data folder)
    unregistered: frozenset  # manifest paths loaded but missing from VERSIONS
    orphans: frozenset       # manifest file names named somewhere but never loaded at module level


def _const(node):
    return node.value if isinstance(node, ast.Constant) else None


def _assignments(tree):
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            yield node.targets[0], node.value


def extract(source):
    tree = ast.parse(source)
    bases, loads, registry, titles, apps = {}, {}, {}, None, None
    for target, value in _assignments(tree):
        if isinstance(target, ast.Subscript) and isinstance(target.value, ast.Name) and target.value.id == 'VERSIONS':
            if isinstance(value, ast.Name):
                registry[_const(target.slice)] = value.id
            continue
        if not isinstance(target, ast.Name):
            continue
        name = target.id
        if isinstance(value, ast.BinOp) and isinstance(value.op, ast.Div) and \
                isinstance(value.left, ast.Name) and value.left.id == 'REPO' and isinstance(_const(value.right), str):
            bases[name] = _const(value.right)
        elif name == 'VERSIONS' and isinstance(value, ast.Dict):
            registry.update({_const(k): v.id for k, v in zip(value.keys, value.values) if isinstance(v, ast.Name)})
        elif name == 'TITLES' and isinstance(value, ast.Dict):
            titles = {_const(k): _const(v) for k, v in zip(value.keys, value.values)}
        elif name == 'APPS' and isinstance(value, ast.Dict):
            apps = {_const(k): tuple(_const(e) for e in v.elts) for k, v in zip(value.keys, value.values)}
        else:
            path = _manifest_path(value, bases)
            if path:
                loads[name] = path
    for what, found in (('base folders (X = REPO / ...)', bases), ('manifest loads', loads),
                        ('VERSIONS', registry), ('TITLES', titles), ('APPS', apps)):
        if not found:
            raise SourceLayoutChanged(f'build_app.py: no {what} found')
    missing = sorted(n for n in registry.values() if n not in loads)
    if missing:
        raise SourceLayoutChanged(f'build_app.py: VERSIONS names {missing}, which are not manifest loads')
    versions = frozenset((path.split('/')[1], key, path) for key, n in registry.items() for path in [loads[n]])
    registered = {loads[n] for n in registry.values()}
    loaded_files = {path.rsplit('/', 1)[1] for path in loads.values()}
    named = {s.rsplit('/', 1)[-1] for s in _strings(tree) if fnmatch(s, '*' + MANIFEST) and not re.search(r'\s', s)}
    return BuildAppFacts(versions, titles, apps, frozenset(set(loads.values()) - registered),
                         frozenset(named - loaded_files))


def _manifest_path(value, bases):
    """'games/totk/assets/expected-v1.0.0.json' when value loads a manifest from a base folder, else None."""
    for node in ast.walk(value):
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div) and isinstance(node.left, ast.Name) \
                and node.left.id in bases and isinstance(_const(node.right), str) and fnmatch(_const(node.right), MANIFEST):
            return f'{bases[node.left.id]}/{_const(node.right)}'
    return None


def _strings(tree):
    return (node.value for node in ast.walk(tree) if isinstance(node, ast.Constant) and isinstance(node.value, str))


def differences(facts, games):
    """Every way the catalog (tools.builder.games.GAMES) and build_app.py's facts disagree, as readable lines."""
    catalog = frozenset((spec.id, v.key, f'{spec.assets_dir}/{v.manifest}') for spec in games.values() for v in spec.versions)
    out = [f'only in build_app.py: {v}' for v in sorted(facts.versions - catalog)]
    out += [f'only in the catalog: {v}' for v in sorted(catalog - facts.versions)]
    out += [f'loaded but not in VERSIONS: {p}' for p in sorted(facts.unregistered)]
    out += [f'named but never loaded: {f}' for f in sorted(facts.orphans)]
    titles = {spec.title_id: spec.id for spec in games.values()}
    if facts.titles != titles:
        out.append(f'TITLES {facts.titles} != catalog {titles}')
    apps = {spec.id: (spec.app_name, spec.bundle_id, spec.data_folder) for spec in games.values()}
    if facts.apps != apps:
        out.append(f'APPS {facts.apps} != catalog {apps}')
    return out

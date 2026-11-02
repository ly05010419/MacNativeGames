"""The check report the Checks tab exports and the CLI's --check-only prints (S2.3; the logs line).

One place knows where the builder's own logs and the game's run logs live, so the window and the
command line cannot disagree: the builder writes ~/Library/Logs/MacNativeGames/builder-<game>-
<date>.log (build_request.reserve_log) and the game launcher writes <game>.log / <game>.previous.log
there (games/totk/app/launcher.m). report_data() turns the panel's results into plain data (JSON
ready, with no Tk and no locale), and text_report() writes it for a person; the input files are
measured, and hashed only when they are small enough that reading them cannot stall the export.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
import hashlib
import json
from pathlib import Path

from tools.builder.storage import human

FORMAT = 1
HASH_LIMIT = 1024 * 1024 * 1024          # 1 GiB: larger files are measured, never read for a report
BUILDER_KEEP = 5                         # the latest builder logs a report lists


def logs_dir(home):
    """Where the builder's logs and the game's run logs are: ~/Library/Logs/MacNativeGames (S2.3)."""
    return Path(home) / 'Library' / 'Logs' / 'MacNativeGames'


@dataclass(frozen=True)
class LogFile:
    kind: str                    # 'builder' | 'game'
    path: Path
    exists: bool
    size: int = 0
    modified: str = ''


def _log(path, kind):
    path = Path(path)
    try:
        info = path.stat()
    except OSError:
        return LogFile(kind, path, False)
    return LogFile(kind, path, True, info.st_size, datetime.fromtimestamp(info.st_mtime).isoformat(timespec='seconds'))


def log_files(logs_dir, game):
    """The builder's latest logs for this game, then the game's own last and current run log."""
    logs_dir = Path(logs_dir)
    builder = []
    try:
        found = sorted(logs_dir.glob(f'builder-{game}-*.log'), key=lambda p: p.stat().st_mtime, reverse=True)
    except OSError:
        found = []
    builder = [_log(path, 'builder') for path in found[:BUILDER_KEEP]]
    game_logs = [_log(logs_dir / f'{game}.log', 'game'),
                 _log(logs_dir / f'{game}.previous.log', 'game')]
    return tuple(builder + game_logs)


def log_lines(logs_dir, game):
    """The log lines the CLI prints after the checks: the folder, then each log file or one line
    saying there is none yet. Shared with the window's summary so both can never drift."""
    logs_dir = Path(logs_dir)
    files = log_files(logs_dir, game)
    present = [f for f in files if f.exists]
    if not present:
        return (f'{logs_dir} (no log yet)',)
    return tuple(f'{f.kind}: {f.path}' for f in present)


def _file_facts(field, path, problems):
    path = Path(path).expanduser()
    facts = {'field': field, 'path': str(path)}
    try:
        info = path.stat()
    except OSError:
        facts.update(exists=False)
        return facts
    facts['exists'] = True
    if path.is_dir():
        facts['kind'] = 'folder'
        return facts
    facts['kind'] = 'file'
    facts['size'] = info.st_size
    facts['size_human'] = human(info.st_size)
    if info.st_size > HASH_LIMIT:
        facts['sha256'] = None
        facts['hash_skipped'] = f'larger than {human(HASH_LIMIT)}'
        return facts
    try:
        digest = hashlib.sha256()
        with open(path, 'rb') as f:
            for block in iter(lambda: f.read(1024 * 1024), b''):
                digest.update(block)
        facts['sha256'] = digest.hexdigest()
    except OSError as error:
        facts['sha256'] = None
        problems.append(f'{field}: {error.strerror or error}')
    return facts


def report_data(spec, results, logs_dir, paths, now, labels=None):
    """The report as plain data. ``results`` are CheckResult-like (id, status, message_id, detail,
    fix); ``labels`` maps a check id to its shown name (the window passes strings.text)."""
    labels = labels or {}
    fields = ('game', 'keys', 'update', 'firmware', 'shared_fonts')
    problems = []
    inputs = [_file_facts(field, paths[field], problems) for field in fields if paths.get(field)]
    files = log_files(logs_dir, spec.id)
    rendered = []
    for r in results:
        rendered.append({
            'id': r.id,
            'label': labels.get(r.id, r.id),
            'status': r.status.name,
            'message_id': r.message_id,
            'detail': r.detail,
            'fix': r.fix,
        })
    return {
        'format': FORMAT,
        'game': {'id': spec.id, 'name': spec.display_name, 'title_id': spec.title_id},
        'time': datetime.fromtimestamp(now).isoformat(timespec='seconds'),
        'logs_dir': str(logs_dir),
        'logs': {
            'builder': [_log_dict(f) for f in files if f.kind == 'builder'],
            'game': [_log_dict(f) for f in files if f.kind == 'game'],
        },
        'inputs': inputs,
        'checks': rendered,
        'problems': problems,
    }


def _log_dict(log):
    return {'path': str(log.path), 'exists': log.exists,
            'size': log.size if log.exists else 0, 'modified': log.modified}


def text_report(data):
    """A person's report: what was checked, what is wrong, and where to look afterwards."""
    lines = [f"MacNativeGames builder check report  (format {data['format']})",
             f"game: {data['game']['name']} ({data['game']['id']}, {data['game']['title_id']})",
             f"time: {data['time']}",
             f"logs: {data['logs_dir']}", '']
    for kind, title in (('builder', 'builder logs'), ('game', 'game logs')):
        lines.append(f'{title}:')
        logs = data['logs'][kind]
        if not logs:
            lines.append('  (none)')
        for log in logs:
            state = f"{human(log['size'])}, {log['modified']}" if log['exists'] else 'missing'
            lines.append(f"  {log['path']}  ({state})")
    if data['inputs']:
        lines.append('')
        lines.append('input files:')
        for item in data['inputs']:
            if not item.get('exists'):
                lines.append(f"  {item['field']}: {item['path']}  (missing)")
            elif item.get('kind') == 'folder':
                lines.append(f"  {item['field']}: {item['path']}  (folder)")
            else:
                hash_line = item.get('sha256') or f"sha256 skipped ({item.get('hash_skipped', 'not computed')})"
                lines.append(f"  {item['field']}: {item['path']}  ({item['size_human']}; {hash_line})")
    lines.append('')
    lines.append('checks:')
    for check in data['checks']:
        if check['status'] == 'OK':
            lines.append(f"  OK     {check['label']}")
            continue
        detail = f" -- {check['detail']}" if check['detail'] else ''
        lines.append(f"  {check['status']:<6} {check['label']}{detail}  [{check['message_id']}]")
        if check['fix']:
            lines.append(f"         fix: {check['fix']}")
    if data['problems']:
        lines.append('')
        lines.append('problems while reading the files:')
        lines.extend(f'  {problem}' for problem in data['problems'])
    return '\n'.join(lines) + '\n'


def json_report(data):
    return json.dumps(data, indent=2, sort_keys=True) + '\n'


def render(data, path):
    """Write the report in the form the chosen name asks for: .json, anything else is text."""
    path = Path(path)
    body = json_report(data) if path.suffix.lower() == '.json' else text_report(data)
    path.parent.mkdir(parents=True, exist_ok=True)       # the Logs folder may not exist yet
    path.write_text(body, encoding='utf-8')
    return path


def suggested_path(logs_dir, game, now):
    """Where the export goes when the player just accepts the dialog: the builder's own log folder."""
    stamp = datetime.fromtimestamp(now).strftime('%Y%m%d-%H%M%S')
    return Path(logs_dir) / f'check-{game}-{stamp}.json'

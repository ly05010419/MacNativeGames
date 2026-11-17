"""The Launcher's "Check shaders" (user 2026-11-13): what the game's newest shared list on GitHub adds to an installed
app, with nothing changed (shader_update.py check). One JSON object: {"ok": true, "result": {...}} or {"ok": false,
"error": "<words>"}; the update itself is a build job (build_job.command, request {"update": <app>, "shaders": true})."""
import json
from pathlib import Path
import sys
import tempfile

from tools.builder_jobs import repo
from tools.builder_jobs.text import t


def line(r, updatable=True):
    """The result in words, as the window shows it. updatable: whether the window can also offer to apply the list;
    when it cannot, the words say what takes it in instead, so a count never appears beside no button at all."""
    if not r['new_shaders'] and not r['new_pipelines']:
        return t('Shaders and pipelines are up to date with the newest list.', '着色器和管线已经是最新清单的了。')
    return t(f"{r['new_shaders']} new shaders and {r['new_pipelines']} new pipelines in the newest list"
             + (f" ({r['waiting']} more pipelines need the new shaders)" if r['waiting'] else '')
             + ('' if updatable else '; rebuild the app to take them in'),
             f"最新清单里有新着色器 {r['new_shaders']} 个、新管线 {r['new_pipelines']} 条"
             + (f"（另有 {r['waiting']} 条管线要等新着色器）" if r['waiting'] else '')
             + ('' if updatable else '；重新构建应用即可用上'))


def check(app):
    if str(repo.REPO) not in sys.path:
        sys.path.insert(0, str(repo.REPO))
    import shader_update
    try:
        game, _ = shader_update.identity(Path(app).expanduser())
        if game not in shader_update.LISTS:
            raise RuntimeError(t('this game has no shader list to check yet', '这个游戏还不能检查着色器'))
        with tempfile.TemporaryDirectory() as tmp:
            r = shader_update.check(Path(app).expanduser(), shader_update.download(game, tmp))
    except Exception as e:   # the reason in words for the window (a download, an app the check cannot read)
        return {'ok': False, 'error': str(e)}
    # Only offer the update for a game shader_update.py can assemble: a game packaged by its own build.py takes a new
    # list through a rebuild, so offering "Update shaders" there would start a build job that stops with an error.
    updatable = r['game'] in shader_update.UPDATES
    return {'ok': True, 'result': {**r, 'line': line(r, updatable),
                                   'can_update': updatable and bool(r['new_shaders'] or r['new_pipelines'])}}


def main(app):
    print(json.dumps(check(app), ensure_ascii=False))
    return 0

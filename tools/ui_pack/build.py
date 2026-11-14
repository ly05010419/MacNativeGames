"""The build: one pack picture per key of the key list, gated; the manifest decides what is rebuilt, rewritten or deleted.
Per key: skip (option, colour art, flat, pure gradient) | fit shapes (recon) -> render (pack) -> gate -> PNG. Method revision: bump
REVISION whenever the method, the gate or its thresholds change; every picture is then rebuilt."""
import concurrent.futures
import hashlib
import io
import json
import os
from pathlib import Path

import numpy as np
from PIL import Image

from tools.ui_pack import gate, manifest, pack, recon
from tools.ui_pack.errors import UiPackError
from tools.ui_pack.harvest import read_harvest
from tools.ui_pack.keylist import Entry, draw_scale

REVISION = 'v3.2-contourpy'
GRADIENT_MIN = 0.5     # a mask whose sharpest 4-neighbour step is below this fraction of the range of every used channel is a pure gradient
COLOUR = 'colour art: the shape method is for masks'


def step_fraction(c):
    """The sharpest 4-neighbour step of a channel over its range."""
    return max(np.abs(np.diff(c, axis=0)).max(initial=0), np.abs(np.diff(c, axis=1)).max(initial=0)) / np.ptp(c)


def digest(blob, cls, options, entry, keylist, screen_height):
    scale = entry.scale if entry.scale is not None else keylist.default_scale
    text = '\0'.join([REVISION, cls, json.dumps(options, sort_keys=True), repr(scale), str(keylist.reference_height), str(screen_height)])
    return hashlib.sha256(blob + b'\0' + text.encode()).hexdigest()[:16]


def make(source, cls, options, sd):
    """The result of one key: dict(status, reason, F, gate, png (bytes or None)). Runs in a worker process."""
    try:
        if 'skip' in options:
            return dict(status='skipped', reason=options['skip'])
        im = Image.open(source)
        if cls == 'colour' or im.mode not in ('L', 'RGB'):
            return dict(status='skipped', reason=COLOUR)
        src = np.asarray(im, float); src = src[..., None] if src.ndim == 2 else src
        ch = pack.used_channels(src)
        if not ch:
            return dict(status='skipped', reason='flat: no channel varies by more than 24 levels')
        fr = max(step_fraction(src[..., i]) for i in ch)
        if fr < GRADIENT_MIN:
            return dict(status='skipped', reason=f'pure gradient: the sharpest step is {fr:.0%} of the range (below {GRADIENT_MIN:.0%})')
        res = recon.fit_key(src, options)
        F = options.get('F', pack.factor(src))
        img, dn, npatch = pack.render(src, res, F, sd, options)
        m = gate.measure(src, res, img, dn, F, sd)
        cell = gate.summary(m) + f' patch={npatch}'
        bad = gate.verdict(m); accept = options.get('accept', {})
        left = {c: v for c, v in bad.items() if c not in accept}
        if left:
            return dict(status='failed', reason='gate: ' + '; '.join(f'{c}: {t}' for c, (_, t) in left.items()), F=F, gate=cell)
        buf = io.BytesIO()
        Image.fromarray(img[..., 0] if im.mode == 'L' else img, im.mode).save(buf, 'PNG')
        note = '; '.join(f'accepted {c}: {accept[c]}' for c in bad)
        return dict(status='written', reason=note, F=F, gate=cell, png=buf.getvalue())
    except Exception as error:   # a fault in one key must not stop the others
        return dict(status='failed', reason=f'error: {type(error).__name__}: {error}')


def write_png(path, data):
    tmp = Path(str(path) + '.tmp')
    tmp.write_bytes(data)
    os.replace(tmp, path)


def build(harvest, out, keylist, screen_height, only=None, all_keys=False, force=False, dry_run=False, jobs=1, log=print):
    """Returns the number of keys that failed. A picture is written, rewritten or deleted only when the manifest owns it."""
    harvest, out = Path(harvest), Path(out)
    rows = {r.key: r for r in read_harvest(harvest / 'harvest.tsv')}
    listed = keylist.entries
    missing = sorted(set(listed) - set(rows))
    if missing:
        log(f'note: {len(missing)} of the {len(listed)} listed textures are not in the harvest: ' + ', '.join(missing))
    targets = {k: listed[k] for k in sorted(listed) if k in rows}
    if all_keys:
        targets.update({k: Entry(k, None, {}, '') for k in sorted(rows) if k not in listed})
    elif len(rows) > len(targets):
        log(f'{len(rows) - len(targets)} harvested textures are not in the key list: not built (--all builds them with the default options)')
    if only is not None:
        unknown = sorted(set(only) - set(targets))
        if unknown:
            raise UiPackError('not in the harvest and the key list (--all adds the unlisted ones): ' + ', '.join(unknown))
        targets = {k: v for k, v in targets.items() if k in only}
    table = manifest.read(out) if out.is_dir() else {}
    counts = dict(built=0, unchanged=0, left_alone=0, deleted=0)
    kinds = dict(written=0, skipped=0, failed=0)
    todo = []
    for key, entry in targets.items():
        source, target = harvest / f'{key}.png', out / f'{key}.png'
        old = table.get(key)
        owned = old is not None and old.status in manifest.OWNED
        if not source.is_file():
            kinds['failed'] += 1; log(f'{key}: FAILED: {source} not found'); continue
        if target.exists() and not owned:
            counts['left_alone'] += 1; log(f'{key}: left alone ({target.name} is not in {manifest.MANIFEST}; another tool wrote it)'); continue
        sd = draw_scale(entry, keylist, screen_height)
        dig = digest(source.read_bytes(), rows[key].cls, entry.options, entry, keylist, screen_height)
        if not force and old is not None and old.digest == dig and (old.status != 'written' or target.is_file()):
            counts['unchanged'] += 1; kinds[old.status] += 1
            log(f'{key}: unchanged ({old.status}{": " + old.reason if old.reason and old.status != "written" else ""})'); continue
        todo.append((key, entry, source, target, sd, dig, owned))
    stale = [] if only is not None else sorted(k for k, r in table.items() if r.status in manifest.OWNED and k not in targets and k not in missing)
    if not dry_run:
        out.mkdir(parents=True, exist_ok=True)
    for key in stale:
        counts['deleted'] += 1
        log(f'{key}: {"would delete" if dry_run else "deleted"} (no longer a target of this build; the manifest owned it)')
        if not dry_run:
            (out / f'{key}.png').unlink(missing_ok=True); del table[key]

    def finish(item, result):
        key, entry, source, target, sd, dig, owned = item
        status = result['status']
        if status == 'written':
            write_png(target, result['png']); counts['built'] += 1; kinds['written'] += 1
        else:
            if owned: target.unlink(missing_ok=True)
            kinds[status] += 1
        table[key] = manifest.Row(status, result.get('reason', ''), str(result.get('F', '')), f'{sd:.4g}', str(screen_height), result.get('gate', ''), dig)
        manifest.write(out, table)
        log(f'{key}: {status}' + (f' x{result["F"]}' if status == 'written' else '') + f' s_d {sd:.3g}' + (f' [{result["gate"]}]' if result.get('gate') else '')
            + (f': {result["reason"]}' if result.get('reason') else ''))

    if dry_run:
        for key, *_ in todo:
            counts['built'] += 1; log(f'{key}: would build')
    elif jobs > 1 and len(todo) > 1:
        todo.sort(key=lambda t: -t[2].stat().st_size)    # big sources first: they take longest
        with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as pool:
            futures = {pool.submit(make, str(t[2]), rows[t[0]].cls, t[1].options, t[4]): t for t in todo}
            for fut in concurrent.futures.as_completed(futures):
                finish(futures[fut], fut.result())
    else:
        for t in todo:
            finish(t, make(str(t[2]), rows[t[0]].cls, t[1].options, t[4]))
    if not dry_run:
        manifest.write(out, table)
        manifest.remove_old(out)
    tail = ((f'; {counts["deleted"]} stale pictures {"to delete" if dry_run else "deleted"}' if counts['deleted'] else '')
            + (f'; {counts["left_alone"]} pictures left alone (not in the manifest)' if counts['left_alone'] else ''))
    if dry_run:
        log(f'dry run: {counts["built"]} to build, {counts["unchanged"]} unchanged (of which skipped {kinds["skipped"]}, failed {kinds["failed"]}){tail}')
    else:
        log(f'written {kinds["written"]} ({counts["built"]} built, {counts["unchanged"]} unchanged), skipped {kinds["skipped"]}, failed {kinds["failed"]}{tail}')
    return kinds['failed']

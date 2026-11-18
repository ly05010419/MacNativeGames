"""How far a build is, from how long the same steps took on this Mac last time (no guessed durations).

A build's steps are the logs build_app.py writes, one per tool it runs (run(cmd, log): logs/<step>.log), in the order
they start. After a build that succeeded, record() keeps each step's name and seconds. The next build's fraction is the
time the steps before the current one took last time, plus the current step's time so far (capped at what it took),
over last time's total: a step this build skips (its inputs unchanged) counts as done, since the next step is further
on. Without a record, the fraction is the stage's start (build_app.py's 'i/4' lines) and there is no time left: the
window says it is an estimate. Contract: no method raises for a missing or damaged record; it is treated as none."""
import json
from pathlib import Path
import time

# Where a first build's bar stands when each of build_app.py's four stages starts (code, data, shaders, app): a fixed
# split, not a measurement, used only until this Mac has its own record; the window marks it as an estimate.
STAGE_STARTS = {1: 0.0, 2: 0.10, 3: 0.30, 4: 0.85}


def record_path(folder, game, platform):
    return Path(folder) / f'{game}-{platform}.json'


def load(path):
    try:
        data = json.loads(Path(path).read_text())
        steps = [(str(n), float(s)) for n, s in data['steps']]
        return steps if steps and sum(s for _, s in steps) > 0 else None
    except (OSError, ValueError, KeyError, TypeError):
        return None


def record(path, steps, end):
    """steps: [(name, start time)] in order; end: when the build finished."""
    timed = [(name, max(0.0, (steps[i + 1][1] if i + 1 < len(steps) else end) - start))
             for i, (name, start) in enumerate(steps)]
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps({'steps': timed, 'total': end - steps[0][1] if steps else 0,
                               'when': time.strftime('%Y-%m-%dT%H:%M:%S')}))
    tmp.replace(path)


# A running step's own counter ("400 / 42848 shaders translated") is its time left once it has run this long and counted
# something; before that its rate is the start-up, not the work.
RATE_AFTER = 5.0


def counted_left(since, count):
    """Seconds the running step still needs at the rate it has gone so far, or None without a usable counter."""
    if not count or since < RATE_AFTER:
        return None
    done, total = count
    return since * (total - done) / done if 0 < done <= total else None


class Estimator:
    """The time left of a build (user 2026-11-17: "预估 时间 也不对 没有根据 真实情况来 总是上次的情况"): the running step
    from its own counter, else from last time while it has not run longer than that; the steps after it from last time,
    until they start and count. A step with neither (new to the record, or past last time without a counter) has no
    honest total left: None, not a number that stands still."""

    def __init__(self, last):
        self.last = last                        # [(name, seconds)] or None
        self.order = {n: i for i, (n, _) in enumerate(last or [])}
        self.total = sum(s for _, s in last or [])
        self.fraction = 0.0

    def update(self, current, since, stage, count=None):
        """current: the running step's name (or None), since: seconds it has run, stage: build_app.py's stage (1-4),
        count: the step's (done, total) counter or None -> (fraction 0..1, seconds left or None, measured: whether the
        fraction comes from this Mac's record, the running step's seconds left or None)."""
        step_left = counted_left(since, count)
        if self.last and current in self.order:
            i = self.order[current]
            expected = self.last[i][1]
            if step_left is None and since <= expected:
                step_left = expected - since
            before = sum(s for _, s in self.last[:i])
            now = before + min(since, expected)
            self.fraction = max(self.fraction, min(0.99, now / self.total))
            after = sum(s for _, s in self.last[i + 1:])
            return self.fraction, None if step_left is None else step_left + after, True, step_left
        if self.last:   # a step last time did not run: hold the fraction, no total left to promise
            return self.fraction, None, True, step_left
        self.fraction = max(self.fraction, STAGE_STARTS.get(stage, 0.0))
        return self.fraction, None, False, step_left

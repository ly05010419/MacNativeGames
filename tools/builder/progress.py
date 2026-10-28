"""The only place in the builder that knows what build_app.py prints (S1.4): one output line in, one event out.

build_app.py prints through say() ('[HH:MM:SS] message') and, on failure, 'ERROR: message' on stderr. parse_line is pure
and keeps no state: joining a multi-line error or following the stages is the controller's job (S1.7). Numbers are
matched with bounded digit counts, so int() never sees one it could refuse; the only exception parse_line raises is
TypeError for a value that is not a str."""
from dataclasses import dataclass
import re

KNOWN_STAGES = ('code', 'data', 'update', 'shaders', 'app')   # build_app.py's 'i/4 <name>:' prefixes
STAGE_TOTAL = 4


@dataclass(frozen=True)
class Stage:
    index: int
    total: int
    name: str
    text: str
    raw: str
    time: str | None


@dataclass(frozen=True)
class Heartbeat:
    minutes: int
    raw: str
    time: str | None


@dataclass(frozen=True)
class GameIdentified:
    game: str               # 'totk' or 'botw'
    version: str | None     # BotW names it; the TOTK line does not
    raw: str
    time: str | None


@dataclass(frozen=True)
class UpdateIdentified:
    version: str
    raw: str
    time: str | None


@dataclass(frozen=True)
class Done:
    minutes: int
    app: str
    raw: str
    time: str | None


@dataclass(frozen=True)
class Failure:
    text: str               # the first line of the error, without 'ERROR: '
    raw: str
    time: str | None


@dataclass(frozen=True)
class Line:
    text: str               # without the '[HH:MM:SS] ' prefix when there is one
    raw: str
    time: str | None


_TIMESTAMPED = re.compile(r'\[([0-9]{2}:[0-9]{2}:[0-9]{2})\] (.*)', re.DOTALL)
_STAGE = re.compile(r'([0-9]{1,3})/([0-9]{1,3}) ([a-z]+): (.*)', re.DOTALL)
_HEARTBEAT = re.compile(r' {3}\.\.\. still running \(([0-9]{1,6}) min\)')
_BOTW = re.compile(r'Breath of the Wild (\S+); work folder .*', re.DOTALL)        # build_app.py since origin/main 5281186
_TOTK = re.compile(r'work folder .* \([0-9]+ GB free on its volume\)', re.DOTALL)
_UPDATE = re.compile(r'update identified by verified ExeFS hashes: (?:Breath of the Wild )?(\S+)')   # TOTK, or BotW 1.9.0
_DONE = re.compile(r'done in ([0-9]{1,6}) min: (.+)', re.DOTALL)


def _strip_line_ending(line):
    if line.endswith('\r\n'):
        return line[:-2]
    if line.endswith('\n'):
        return line[:-1]
    return line


def parse_line(line):
    if not isinstance(line, str):
        raise TypeError(f'parse_line expects a str, not {type(line).__name__}')
    raw = _strip_line_ending(line)
    if raw.startswith('ERROR: '):
        return Failure(raw[len('ERROR: '):], raw, None)
    stamped = _TIMESTAMPED.fullmatch(raw)
    if not stamped:
        return Line(raw, raw, None)
    time, message = stamped.groups()
    return _message_event(message, raw, time)


def _message_event(message, raw, time):
    if m := _STAGE.fullmatch(message):
        index, total = int(m[1]), int(m[2])
        if 1 <= index <= total:
            return Stage(index, total, m[3], m[4], raw, time)
        return Line(message, raw, time)
    if m := _HEARTBEAT.fullmatch(message):
        return Heartbeat(int(m[1]), raw, time)
    if m := _BOTW.fullmatch(message):
        return GameIdentified('botw', m[1], raw, time)
    if _TOTK.fullmatch(message):
        return GameIdentified('totk', None, raw, time)      # only the TOTK path prints it (build_app.py l. 1653)
    if m := _UPDATE.fullmatch(message):
        return UpdateIdentified(m[1], raw, time)
    if m := _DONE.fullmatch(message):
        return Done(int(m[1]), m[2], raw, time)
    return Line(message, raw, time)

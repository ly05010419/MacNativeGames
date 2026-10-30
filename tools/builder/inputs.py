"""Whether the chosen input files can build the chosen game, before anything runs (pure; no I/O).

Contract (S1.1, C3): every block rule is evaluated on its own, blocks accumulate in rule order, no permissive case
overrides a block, and notes are given only when nothing blocks. The rules restate build_app.py's main() and
parse_args(), plus the README's Breath of the Wild section."""
from dataclasses import dataclass
from enum import Enum

from tools.builder.games import GameSpec


class Status(Enum):
    OK = 'ok'
    BLOCK = 'block'


class BaseKind(Enum):
    XCI = 'xci'
    NSP = 'nsp'
    NSZ = 'nsz'
    XCZ = 'xcz'
    UNKNOWN = 'unknown'


class UpdateKind(Enum):
    NONE = 'none'
    NSP = 'nsp'
    NSZ = 'nsz'
    XCI = 'xci'
    UNKNOWN = 'unknown'


class Fonts(Enum):
    NONE = 'none'
    FIRMWARE = 'firmware'
    SHARED = 'shared'
    BOTH = 'both'


@dataclass(frozen=True)
class InputChoice:
    base_kind: BaseKind
    update_kind: UpdateKind = UpdateKind.NONE
    fonts: Fonts = Fonts.NONE
    fonts_cached: bool = False


@dataclass(frozen=True)
class InputVerdict:
    status: Status
    reasons: tuple[str, ...]
    notes: tuple[str, ...]


def _fonts_required(spec, choice):
    """build_app.py stops an NSP without fonts; a BotW card has no system update partition (README)."""
    return spec.id == 'botw' or choice.base_kind is BaseKind.NSP


def _fonts_missing(spec, choice):
    return _fonts_required(spec, choice) and choice.fonts is Fonts.NONE


# (reason, fires) in rule order: the order of InputVerdict.reasons.
BLOCK_RULES = (
    ('base_kind', lambda spec, c: c.base_kind not in (BaseKind.XCI, BaseKind.NSP)),
    ('update_kind', lambda spec, c: c.update_kind not in (UpdateKind.NONE, UpdateKind.NSP, UpdateKind.NSZ)),
    ('fonts_both', lambda spec, c: c.fonts is Fonts.BOTH),
    ('fonts_needed', lambda spec, c: _fonts_missing(spec, c) and not c.fonts_cached),
    ('botw_needs_update', lambda spec, c: spec.id == 'botw' and c.base_kind is BaseKind.NSP
                                          and c.update_kind is UpdateKind.NONE),
)

NOTE_RULES = (
    ('fonts_cached', lambda spec, c: _fonts_missing(spec, c) and c.fonts_cached),
)


def evaluate_inputs(spec: GameSpec, choice: InputChoice) -> InputVerdict:
    reasons = tuple(reason for reason, fires in BLOCK_RULES if fires(spec, choice))
    if reasons:
        return InputVerdict(Status.BLOCK, reasons, ())
    notes = tuple(note for note, applies in NOTE_RULES if applies(spec, choice))
    return InputVerdict(Status.OK, (), notes)

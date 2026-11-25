"""Which game a .nup holds, read from its package table (the table's SHA-256 is checked first; no game data is read).
Issue #2333: every .nup was taken for Ocarina of Time by its extension, a Mario Kart World update among them."""
from pathlib import Path
import struct
import sys

_READER = str(Path(__file__).resolve().parents[1] / 'games/oot/third_party/nup')
if _READER not in sys.path:
    sys.path.insert(0, _READER)

from nup.format import KIND_BASE, KIND_UPDATE, NupError  # noqa: E402
from nup.read import NupReader  # noqa: E402

OOT_TITLE = '0400486024DEC000'
OOT_VERSION = 131072   # the version games/oot/scripts/import_nup.py imports
BANANZA_TITLE = '040074A01BF12000'
BANANZA_VERSION = 458752   # its 3.2.0 update: build_app.py NS2, games/dkb/assets/expected-v458752.json
BUILT = {OOT_TITLE: OOT_VERSION, BANANZA_TITLE: BANANZA_VERSION}
READ_ERRORS = (NupError, OSError, ValueError, struct.error, IndexError)


def version_name(version, title=OOT_TITLE):
    """Ocarina of Time's 2.0.0. Another game's title version is no display version (Bananza's 3.2.0 is 458752): said as it is."""
    if title != OOT_TITLE:
        return f'v{version}'
    return f'{version >> 16}.{(version >> 8) & 0xFF}.{version & 0xFF}'


def read(path):
    """(the application's title ID, the versions of its base and update packages). More or fewer than one game raises."""
    with NupReader(str(path)) as reader:
        apps = [p for p in reader.packages if p.kind in (KIND_BASE, KIND_UPDATE)]
    titles = {f'{p.title_id & ~0xFFF:016X}' for p in apps}
    if len(titles) != 1:
        raise NupError(f'expected one game in the package, found {len(titles)}')
    return titles.pop(), sorted({p.version for p in apps})


def builds(title, versions):
    """Ocarina of Time and Donkey Kong Bananza, at the version each build takes, are built from a .nup."""
    return title in BUILT and BUILT[title] in versions

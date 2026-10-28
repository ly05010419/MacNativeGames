"""Mario Kart 8 Deluxe identity shared by the build scripts.

The update's Program NCA carries the base title ID; the update NSP's own title ID only names its container.
"""
import json
from pathlib import Path

GAME = Path(__file__).resolve().parents[1]
TITLE = '0100152000022000'
UPDATE_TITLE = '0100152000022800'
AOC_TITLE = '0100152000023001'      # Booster Course Pass: contents.dat and version.txt only, the courses are in the update
NAME = 'Mario Kart 8 Deluxe'
VERSION = '4.0.1'                   # v1507328, the only build this port accepts
EXPECTED_PATH = GAME / 'assets/expected-4.0.1.json'


def expected():
    return json.loads(EXPECTED_PATH.read_text())

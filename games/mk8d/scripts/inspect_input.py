#!/usr/bin/env python3
"""Inspect a Mario Kart 8 Deluxe XCI/NSP without extracting game content or printing keys."""

import argparse
import sys
from collections import Counter
from pathlib import Path

from mk8d import AOC_TITLE, TITLE as BASE_TITLE, UPDATE_TITLE


def inspect(game: Path, keys_path: Path) -> int:
    if not game.is_file():
        print("Game file was not found.", file=sys.stderr)
        return 2
    if not keys_path.is_file():
        print("Key file was not found.", file=sys.stderr)
        return 2

    scripts = Path(__file__).resolve().parents[1] / "third_party" / "totk-scripts"
    sys.path.insert(0, str(scripts))
    try:
        from extract_exefs import container_entries, load_keys, nca_inventory
    except ImportError as error:
        print(f"Missing parser dependency: {error.name}. Install it in this Python environment.", file=sys.stderr)
        return 2

    try:
        keys = load_keys(keys_path)
        with game.open("rb") as source:
            kind, entries = container_entries(source, game.stat().st_size)
            records = nca_inventory(source, entries, keys)
    except (OSError, ValueError) as error:
        print(f"Could not inspect container: {error}", file=sys.stderr)
        return 2

    ids = Counter(record["title_id"] for record, _ in records)
    print(f"Container: {kind.upper()}")
    print(f"NCA files: {len(records)}")
    for title_id, count in sorted(ids.items()):
        label = {BASE_TITLE: " (base / program)", UPDATE_TITLE: " (update)", AOC_TITLE: " (Booster Course Pass)"}.get(title_id, "")
        print(f"Title ID: {title_id}{label}; NCA count: {count}")
    if BASE_TITLE not in ids and UPDATE_TITLE not in ids:
        print("This container does not contain the expected Mario Kart 8 Deluxe title ID.", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", type=Path, help="Mario Kart 8 Deluxe XCI or NSP file")
    parser.add_argument("--keys", type=Path, required=True, help="prod.keys from your own Switch")
    args = parser.parse_args()
    return inspect(args.game, args.keys)


if __name__ == "__main__":
    raise SystemExit(main())

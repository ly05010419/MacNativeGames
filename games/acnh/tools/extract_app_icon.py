#!/usr/bin/env python3
"""Make the app icon (AppIcon.icns) from the update's own title icon (reported in play, PR #1305: the app showed no icon).

The update NSP's Control NCA holds icon_<language>.dat, a 256x256 JPEG: it is read with the same bounded Control RomFS
reader and IVFC check that prepare.py uses for control.nacp, the NACP must be the one prepare.py verified, and sips and
iconutil (part of macOS) make the .icns. package_native_app.py bundles <native work>/build/AppIcon.icns. The icon is
game-derived: the output must stay in the ignored work folder (tools/common.py outside_repository)."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve()
ROOT = HERE.parents[3]
sys.path.insert(0, str(ROOT))

from games.acnh.prepare import TITLE_ID, require   # noqa: E402
from games.acnh.tools.common import outside_repository   # noqa: E402
from games.acnh.tools.extract_exefs import load_keys, container_entries, common_tickets, nca_inventory   # noqa: E402
from games.acnh.tools.extract_romfs import index_romfs   # noqa: E402
from games.acnh.tools.extract_shared_fonts import Slice, nca_romfs   # noqa: E402

SIZES = [(16, "16x16"), (32, "16x16@2x"), (32, "32x32"), (64, "32x32@2x"), (128, "128x128"), (256, "128x128@2x"),
         (256, "256x256"), (512, "256x256@2x"), (512, "512x512"), (1024, "512x512@2x")]


def title_icon(update, keys_path, nacp_expected):
    """The update's title icon (American English first) as JPEG bytes, from a Control RomFS whose NACP is nacp_expected."""
    keys = load_keys(keys_path)
    with update.open("rb") as stream:
        _, entries = container_entries(stream, update.stat().st_size)
        tickets = common_tickets(stream, entries)
        controls = [record for record, _ in nca_inventory(stream, entries, keys)
                    if record["content_type"] == 2 and record["title_id"] == TITLE_ID]
    require(len(controls) == 1, "Expected exactly one Control NCA for ACNH")
    with tempfile.TemporaryDirectory(prefix="control-") as temporary:
        image = Path(temporary) / "romfs.bin"
        nca_romfs(Slice(update, controls[0]["nca_offset"]), keys, image, tickets)   # all IVFC levels checked
        rows = {row["path"]: row for row in index_romfs(image)}
        with image.open("rb") as data:
            def read(row):
                data.seek(row["offset"]); return data.read(row["size"])
            require("control.nacp" in rows and read(rows["control.nacp"]) == nacp_expected,
                    "This update's Control NACP is not the one prepare.py verified")
            icons = sorted(path for path in rows if path.startswith("icon_") and path.endswith(".dat"))
            require(icons, "The Control RomFS has no title icon")
            jpeg = read(rows["icon_AmericanEnglish.dat" if "icon_AmericanEnglish.dat" in rows else icons[0]])
    require(jpeg.startswith(b"\xff\xd8\xff") and jpeg.endswith(b"\xff\xd9"), "The title icon is not a complete JPEG")
    return jpeg


def make_icns(jpeg, output):
    with tempfile.TemporaryDirectory(prefix="icon-") as temporary:
        source = Path(temporary) / "icon.jpg"; source.write_bytes(jpeg)
        iconset = Path(temporary) / "AppIcon.iconset"; iconset.mkdir()
        for px, name in SIZES:
            subprocess.run(["sips", "-s", "format", "png", "-z", str(px), str(px), str(source), "--out", str(iconset / f"icon_{name}.png")],
                           check=True, capture_output=True)
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(output)], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("update", type=Path, help="the update NSP prepare.py used")
    p.add_argument("--keys", type=Path, required=True)
    p.add_argument("--prepared", type=Path, required=True, help="the prepare.py work folder (update/control.nacp)")
    p.add_argument("--output", type=Path, required=True, help="<native work>/build/AppIcon.icns")
    a = p.parse_args()
    output = outside_repository(a.output, ROOT)
    nacp = (a.prepared / "update/control.nacp").read_bytes()
    make_icns(title_icon(a.update.resolve(), a.keys.resolve(), nacp), output)
    print(f"Title icon -> {output}")


if __name__ == "__main__":
    main()

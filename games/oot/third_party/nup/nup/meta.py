"""NUP package meta block: TLV assembly for per-package metadata.

Builds the per-package meta block (NPDM, build ID, provenance) and the
application-level TLVs (names, icons, display version, SPEC §5.1) that
ride along in the base package's block. Serialization is deterministic:
records are sorted by TLV type and written without padding.
"""

from __future__ import annotations

import struct
from typing import Protocol

from . import format as F


class MetaSource(Protocol):
    """What meta_block needs of a package: its dump-derived metadata."""

    npdm: bytes | None
    build_id: bytes | None


def read_file(path: str) -> bytes:
    with open(path, "rb") as fh:
        return fh.read()


def parse_block(block: bytes) -> dict[int, bytes]:
    """TLV type -> payload for one meta block (first occurrence wins)."""
    out: dict[int, bytes] = {}
    pos = 0
    while pos + 8 <= len(block):
        t, _r, size = F._tlv_s.unpack_from(block, pos)
        if pos + 8 + size > len(block):
            raise F.NupError("meta TLV overruns block")
        out.setdefault(t, bytes(block[pos + 8:pos + 8 + size]))
        pos += 8 + size
    return out


def serialize_block(records: dict[int, bytes]) -> bytes:
    """Deterministic meta block: records sorted by type, no padding."""
    out = bytearray()
    for t in sorted(records):
        payload = records[t]
        out += F._tlv_s.pack(t, 0, len(payload)) + payload
    return bytes(out)


def decode_localized(payload: bytes) -> dict[bytes, bytes]:
    """Parse a LocalizedRecord sequence (SPEC §5.1) into {lang tag: data}."""
    out: dict[bytes, bytes] = {}
    pos = 0
    while pos < len(payload):
        lang_len = payload[pos]
        pos += 1
        lang = payload[pos:pos + lang_len]
        pos += lang_len
        (data_len,) = struct.unpack_from("<I", payload, pos)
        pos += 4
        if pos + data_len > len(payload):
            raise F.NupError("localized record overruns TLV")
        out[lang.lower()] = payload[pos:pos + data_len]
        pos += data_len
    return out


def merge_app_meta(existing: dict[int, bytes], *, name: str | None = None,
                   publisher: str | None = None, display_version: str | None = None,
                   icon: str | None = None, lang_names: dict[str, str] | None = None,
                   lang_icons: dict[str, str] | None = None,
                   clear_name: bool = False, clear_publisher: bool = False,
                   clear_display_version: bool = False,
                   clear_icon: bool = False) -> dict[int, bytes]:
    """Apply edit flags to existing meta records (TLV type -> payload).

    Provided values override; everything else is preserved. Empty lang
    values remove that language from the localized set; --clear-* drops
    the default-language TLV. Package-level TLVs pass through untouched.
    """
    out = dict(existing)
    if clear_name:
        out.pop(F.TLV_TITLE_NAME, None)
    elif name is not None:
        out[F.TLV_TITLE_NAME] = name.encode("utf-8")
    if clear_publisher:
        out.pop(F.TLV_PUBLISHER, None)
    elif publisher is not None:
        out[F.TLV_PUBLISHER] = publisher.encode("utf-8")
    if clear_display_version:
        out.pop(F.TLV_DISPLAY_VERSION, None)
    elif display_version is not None:
        out[F.TLV_DISPLAY_VERSION] = display_version.encode("utf-8")
    if clear_icon:
        out.pop(F.TLV_ICON, None)
    elif icon is not None:
        out[F.TLV_ICON] = read_file(icon)
    for tlv, changes, read_values in ((F.TLV_LOCALIZED_TITLES, lang_names, False),
                                      (F.TLV_LOCALIZED_ICONS, lang_icons, True)):
        if not changes:
            continue
        loc = {lang.decode("ascii"): data
               for lang, data in decode_localized(out[tlv]).items()} if tlv in out else {}
        for lang, value in changes.items():
            if value == "":
                loc.pop(lang.lower(), None)
            else:
                loc[lang.lower()] = read_file(value) if read_values else value.encode("utf-8")
        if loc:
            out[tlv] = _encode_localized(loc)
        else:
            out.pop(tlv, None)
    return out


def meta_block(pkg: MetaSource, dump_label: str, app_extra: bytes = b"") -> bytes:
    """Serialize one package's meta block (SPEC §5)."""
    recs: list[tuple[int, bytes]] = []
    if pkg.npdm:
        recs.append((F.TLV_NPDM, pkg.npdm))
    if pkg.build_id:
        recs.append((F.TLV_BUILD_ID, pkg.build_id))
    recs.append((F.TLV_SOURCE_DUMP, dump_label.encode("utf-8")))
    if app_extra:
        # merge prebuilt application-level TLVs (names/icons/display version)
        pos = 0
        while pos + 8 <= len(app_extra):
            t, _r, size = F._tlv_s.unpack_from(app_extra, pos)
            recs.append((t, app_extra[pos + 8:pos + 8 + size]))
            pos += 8 + size
    recs.sort(key=lambda t: t[0])
    deduped: list[tuple[int, bytes]] = []
    for rec in recs:  # app_extra may repeat per-package types; keep first
        if deduped and deduped[-1][0] == rec[0]:
            continue
        deduped.append(rec)
    out = bytearray()
    for t, payload in deduped:
        out += F._tlv_s.pack(t, 0, len(payload))
        out += payload
    return bytes(out)


def _encode_localized(entries: dict[str, bytes]) -> bytes:
    """Encode {lang tag: data} as a LocalizedRecord sequence (SPEC §5.1)."""
    body = bytearray()
    for lang in sorted(entries):
        tag = lang.lower().encode("ascii")
        if not 0 < len(tag) <= 255:
            raise SystemExit(f"pack: bad language tag {lang!r}")
        data = entries[lang]
        body += bytes([len(tag)]) + tag
        body += struct.pack("<I", len(data)) + data
    return bytes(body)


def app_meta(names: dict[str | None, str] | None, icons: dict[str | None, str] | None,
             display_version: str | None, dump_label: str,
             publisher: str | None = None) -> bytes:
    """Application-level meta TLVs (SPEC §5.1): names/icons localized, plus
    display version, publisher and provenance. Attached to the base package
    (or first)."""
    names = names or {}
    icons = icons or {}
    recs: list[tuple[int, bytes]] = []
    if names.get(None):
        recs.append((F.TLV_TITLE_NAME, names[None].encode("utf-8")))
    if publisher:
        recs.append((F.TLV_PUBLISHER, publisher.encode("utf-8")))
    if names.keys() - {None}:
        localized = {lang: text.encode("utf-8")
                     for lang, text in names.items() if lang is not None}
        recs.append((F.TLV_LOCALIZED_TITLES, _encode_localized(localized)))
    if icons.get(None):
        recs.append((F.TLV_ICON, read_file(icons[None])))
    if icons.keys() - {None}:
        localized = {lang: read_file(path)
                     for lang, path in icons.items() if lang is not None}
        recs.append((F.TLV_LOCALIZED_ICONS, _encode_localized(localized)))
    if display_version:
        recs.append((F.TLV_DISPLAY_VERSION, display_version.encode("utf-8")))
    recs.append((F.TLV_SOURCE_DUMP, dump_label.encode("utf-8")))
    recs.sort(key=lambda t: t[0])  # deterministic order
    out = bytearray()
    for t, payload in recs:
        out += F._tlv_s.pack(t, 0, len(payload)) + payload
    return bytes(out)

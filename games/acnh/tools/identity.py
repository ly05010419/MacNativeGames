#!/usr/bin/env python3
"""Inspect game code identity without claiming an unverified native port."""

import hashlib
import io
import struct
import sys
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import modes

sys.dont_write_bytecode = True

from .extract_exefs import (common_tickets, container_entries, decrypt,
                           load_keys, nca_ctr_key, nca_inventory,
                           partition_entries, program_nca, read_at)


def verified_cnmt(stream, record: dict, header: bytes, keys: dict,
                  tickets: dict, expected_type: int) -> dict | None:
    """Read a small hashed Meta PFS0; leave other Meta layouts unassessed."""
    sections = []
    for index in range(4):
        start, end = struct.unpack_from("<II", header, 0x240 + index * 16)
        if not start:
            continue
        fs = header[0x400 + index * 512:0x600 + index * 512]
        if hashlib.sha256(fs).digest() != header[0x280 + index * 32:0x2a0 + index * 32]:
            raise ValueError("CNMT FS header hash mismatch")
        if fs[2:5] == b"\x01\x02\x03":
            sections.append((start, end, fs, index))
    if not sections:
        return None
    if len(sections) != 1:
        return None
    start, end, fs, _ = sections[0]
    blocksize, levels = struct.unpack_from("<II", fs, 0x28)
    hashoff, hashsize, pfsoff, pfssize = struct.unpack_from("<QQQQ", fs, 0x30)
    if levels != 2 or pfssize > 1024 * 1024:
        return None
    if (blocksize == 0 or
            hashsize != ((pfssize + blocksize - 1) // blocksize) * 32):
        raise ValueError("Invalid CNMT hash layout")
    section_size = (end - start) * 512
    key = nca_ctr_key(header, keys, tickets)

    def section_read(relative: int, size: int) -> bytes:
        if relative + size > section_size:
            raise ValueError("CNMT section read out of bounds")
        absolute = start * 512 + relative
        aligned = absolute & ~15
        iv = fs[0x140:0x148][::-1] + (aligned // 16).to_bytes(8, "big")
        encrypted = read_at(stream, record["nca_offset"] + aligned,
                            size + absolute - aligned)
        return decrypt(key, modes.CTR(iv), encrypted)[absolute - aligned:]

    hashes = section_read(hashoff, hashsize)
    if hashlib.sha256(hashes).digest() != fs[8:40]:
        raise ValueError("CNMT hash-table mismatch")
    pfs = section_read(pfsoff, pfssize)
    for pos in range(0, pfssize, blocksize):
        offset = (pos // blocksize) * 32
        if hashlib.sha256(pfs[pos:pos + blocksize]).digest() != hashes[offset:offset + 32]:
            raise ValueError("CNMT block hash mismatch")
    files = [(name, pfs[offset:offset + size]) for name, offset, size in
             partition_entries(io.BytesIO(pfs), 0, len(pfs)) if name.endswith(".cnmt")]
    if len(files) != 1 or len(files[0][1]) < 0x20:
        raise ValueError("Expected one CNMT file")
    data = files[0][1]
    title, version = struct.unpack_from("<QI", data)
    if title != int(record["title_id"], 16) or data[12] != expected_type:
        raise ValueError("CNMT identity does not match the selected metadata")
    result = {"cnmt_version_raw": version,
              "cnmt_content_type": {0x80: "application", 0x81: "patch", 0x82: "add_on_content"}[expected_type]}
    if expected_type == 0x82:
        extended_size = struct.unpack_from("<H", data, 14)[0]
        if extended_size < 16 or len(data) < 0x20 + extended_size:
            raise ValueError("Truncated add-on content extended header")
        result["application_id"] = f'{struct.unpack_from("<Q", data, 0x20)[0]:016X}'
    return result


def select_program(source: Path, key_file: Path, title_id: str | None = None) -> dict:
    """Validate the container's selected Program and base/update identity."""
    keys = load_keys(key_file)
    with source.open("rb") as stream:
        container, entries = container_entries(stream, source.stat().st_size)
        tickets = common_tickets(stream, entries)
        records = nca_inventory(stream, entries, keys)
    programs = [(record, header) for record, header in records
                if record["content_type"] == 0]
    if title_id is None:
        if len(programs) != 1:
            raise ValueError("Expected one Program NCA; use --title-id for a combined XCI")
        title_id = programs[0][0]["title_id"]
    title_id = title_id.upper()
    selected = [(record, header) for record, header in programs
                if record["title_id"] == title_id]
    if container == "xci" and len(programs) > 1 and any(
        any(header[0x400 + i * 512 + 4] == 4 for i in range(4))
        for _, header in selected
    ):
        raise ValueError("Combined update XCI is unsupported; provide a separate update NSP")
    if len(selected) != 1:
        raise ValueError(f"Expected one Program NCA for title ID {title_id}")
    header = selected[0][1]
    update = any(header[0x400 + i * 512 + 4] == 4 for i in range(4))
    program_nca(records, update=update, title_id=title_id,
                allow_combined=container == "xci" and not update)
    metadata = sorted(record["title_id"] for record, _ in records
                      if record["content_type"] == 1)
    if update and metadata != [f"{int(title_id, 16) + 0x800:016X}"]:
        raise ValueError("Update metadata title ID does not match the base title")
    expected_meta_title = f"{int(title_id, 16) + 0x800:016X}" if update else title_id
    selected_metadata = [(record, header) for record, header in records
                         if record["content_type"] == 1 and
                         record["title_id"] == expected_meta_title]
    with source.open("rb") as stream:
        cnmt = (verified_cnmt(stream, *selected_metadata[0], keys, tickets,
                              0x81 if update else 0x80)
                if selected_metadata else None)
    return {"container": container, "title_id": title_id,
            "package_kind": "update" if update else "base",
            "metadata_title_ids": metadata,
            "cnmt_version_raw": cnmt["cnmt_version_raw"] if cnmt else None,
            "cnmt_content_type": cnmt["cnmt_content_type"] if cnmt else None}

"""Identify NUPs from their package table and stage complete Switch 2 updates."""
import hashlib
import struct

# The vendored reader and validated directory walk are already used by the OoT importer.
from games.oot.scripts.import_nup import NupReader, entries
from nup.format import KIND_BASE, KIND_UPDATE, NO_DIR, NupError

OOT_TITLE = "0400486024DEC000"


class InputError(ValueError):
    def __init__(self, message, title=None):
        super().__init__(message)
        self.title = title


READ_ERRORS = (NupError, OSError, ValueError, struct.error, IndexError)


def select_package(reader, versions):
    packages = [p for p in reader.packages if p.title_id and p.kind in (KIND_BASE, KIND_UPDATE)]
    titles = {f'{p.title_id & ~0xfff:016X}' for p in packages}
    if len(titles) != 1:
        raise InputError('expected one game in the NUP')
    title, = titles
    if title not in versions:
        raise InputError(f'unsupported game (Title ID {title})', title)
    version = versions[title]
    matching = [p for p in packages if p.version == version]
    if len(matching) != 1:
        raise InputError(f'Title ID {title} needs one package at version {version}', title)
    package, = matching
    if title != OOT_TITLE and (package.kind != KIND_UPDATE or NO_DIR in (package.exefs_dir, package.romfs_dir)):
        raise InputError(f'Title ID {title} has no complete ExeFS and RomFS', title)
    return title, package


def inspect(source, versions):
    """Read only the package table. The builder verifies the actual data before use."""
    with NupReader(str(source)) as reader:
        title, package = select_package(reader, versions)
        return title, package.version


def extract_update(source, target, versions, expected_title):
    """Stage the supported update in a fresh dump folder without trusting archive paths."""
    with NupReader(str(source)) as reader:
        title, package = select_package(reader, versions)
        if title != expected_title:
            raise InputError('NUP game changed after identification', title)
        root = target / 'updates' / str(package.version)
        for name, index in (('exefs', package.exefs_dir), ('romfs', package.romfs_dir)):
            directory = root / name
            directory.mkdir(parents=True)
            subdirectories = []
            for relative, member in entries(reader, index, subdirectories):
                try:
                    data = reader.read_file(member)
                except Exception as error:  # the vendored reader can use either zstd implementation
                    raise InputError(f'cannot decode NUP file {name}/{relative}: {error}', title) from error
                if hashlib.sha256(data).digest() != reader.files[member][2]:
                    raise InputError(f'NUP file hash mismatch: {name}/{relative}', title)
                path = directory / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                with path.open('xb') as f:
                    f.write(data)
            for relative in subdirectories:
                (directory / relative).mkdir(parents=True, exist_ok=True)

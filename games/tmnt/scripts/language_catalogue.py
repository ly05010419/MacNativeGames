"""Expose the original Chinese resources through a private, hashed overlay."""
import hashlib


def language_patch(image, entries):
    files = {row['path']: row for row in entries}
    for name in ('title.dat', 'Content/Global/Loc/Chinese.zpbn',
                 'Content/Global/Loc/ChineseTrad.zpbn'):
        if name not in files or files[name]['size'] <= 0:
            raise ValueError(f'Chinese language resource missing: {name}')
    row = files['title.dat']
    if not 0 < row['size'] <= 1024:
        raise ValueError('unsupported title.dat size')
    with image.open('rb') as stream:
        stream.seek(row['offset'])
        original = stream.read(row['size'])
    if len(original) != row['size']:
        raise ValueError('truncated title.dat')
    lines = original.decode('ascii').splitlines(keepends=True)
    if len(lines) != 2 or not lines[1].endswith(('\r\n', '\n')):
        raise ValueError('unsupported title.dat language catalogue')
    ending = '\r\n' if lines[1].endswith('\r\n') else '\n'
    languages = lines[1].rstrip('\r\n').split('|')
    if not all(languages) or lines[0].strip() not in languages:
        raise ValueError('invalid title.dat default language')
    # These are the game's own identifiers, not Switch/BCP 47 codes. Adding
    # zh-cn or zh-tw is silently ignored. Keep the default and all existing
    # languages; the game still chooses the user's language and saves it.
    for code in ('cn', 'zh-hant'):
        if code not in languages:
            languages.append(code)
    patched = (lines[0] + '|'.join(languages) + ending).encode('ascii')
    manifest = (f'title.dat\t{len(patched)}\t{hashlib.sha256(patched).hexdigest()}'
                f'\t{hashlib.sha256(original).hexdigest()}\n').encode('ascii')
    return {'title.dat': patched, 'manifest.tsv': manifest}

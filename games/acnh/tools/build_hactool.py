#!/usr/bin/env python3
"""Build the pinned BKTR reader; adapted from root build_app.py at 1dcf40d.

This local copy keeps ACNH builds independent of other games. Keys never enter
this build helper; the extractor supplies private key files only when merging.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

HACTOOL = ('https://github.com/SciresM/hactool.git', '1d64a83450e025622f3468c28fc4164dad2c5ef6')
BuildError = ValueError


def run(command, log, cwd=None):
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('a') as stream:
        result = subprocess.run([str(c) for c in command], cwd=cwd,
                                stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise BuildError(f'{Path(command[0]).name} failed ({result.returncode}); see {log}')


def git_fetch(url, commit, destination, log):
    if destination.is_symlink():
        raise BuildError('hactool source cache must not be a symlink')
    if (destination / '.git').is_dir():
        current = subprocess.run(['git', '-C', str(destination), 'rev-parse', 'HEAD'],
                                 capture_output=True, text=True)
        if current.returncode == 0:
            if current.stdout.strip() != commit:
                raise BuildError('Existing hactool cache has another commit; choose a new work folder')
            return
    elif destination.exists() and any(destination.iterdir()):
        raise BuildError('Existing hactool cache is not a Git checkout')
    destination.mkdir(parents=True, exist_ok=True)
    run(['git', '-C', destination, 'init', '-q'], log)
    run(['git', '-C', destination, 'fetch', '-q', '--depth', '1', url, commit], log)
    run(['git', '-C', destination, 'checkout', '-q', 'FETCH_HEAD'], log)


def build_hactool(work, logs):
    source = work / 'tools/hactool'
    git_fetch(*HACTOOL, source, logs/'fetch-hactool.log')
    # Add an explicit private title-key file, so base NSP and update tickets do not
    # need command-line keys or changes to the player's HOME/.switch files.
    main = source/'main.c'
    text = main.read_text()
    old = 'FILE *titlekeyfile = open_key_file("title");'
    new = 'FILE *titlekeyfile = getenv("MNG_TITLE_KEYS") ? fopen(getenv("MNG_TITLE_KEYS"), "rb") : open_key_file("title");'
    if new not in text:
        if text.count(old) != 1:
            raise BuildError('pinned hactool title-key hook changed')
        main.write_text(text.replace(old, new))
    # Expose only the bounded Program NCA inside the original XCI/NSP as a
    # seekable FILE. This avoids a second 17 GB base copy during BKTR merging.
    text = main.read_text()
    if '#define _DARWIN_C_SOURCE 1' not in text:
        text = '#define _DARWIN_C_SOURCE 1\n' + text
        main.write_text(text)
    hook = 'static FILE *mng_open_base(const char *path)'
    if hook not in text:
        helper = r'''
typedef struct { FILE *file; off_t start, size, position; } mng_base_range;
static int mng_base_read(void *cookie, char *buffer, int count) {
    mng_base_range *range = cookie;
    off_t left = range->size - range->position;
    if (left < count) count = (int)left;
    if (!count) return 0;
    if (fseeko(range->file, range->start + range->position, SEEK_SET)) return -1;
    int got = (int)fread(buffer, 1, count, range->file);
    if (!got && ferror(range->file)) return -1;
    range->position += got;
    return got;
}
static fpos_t mng_base_seek(void *cookie, fpos_t offset, int whence) {
    mng_base_range *range = cookie;
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? range->position : whence == SEEK_END ? range->size : -1;
    if (base < 0 || offset < -base || offset > range->size - base) return (fpos_t)-1;
    return (fpos_t)(range->position = base + offset);
}
static int mng_base_close(void *cookie) {
    mng_base_range *range = cookie;
    int result = fclose(range->file); free(range); return result;
}
static FILE *mng_open_base(const char *path) {
    const char *start = getenv("MNG_BASE_RANGE_OFFSET"), *size = getenv("MNG_BASE_RANGE_SIZE");
    if (!start && !size) return fopen(path, "rb");
    if (!start || !size) return NULL;
    char *end1, *end2;
    unsigned long long a = strtoull(start, &end1, 10), n = strtoull(size, &end2, 10);
    if (!*start || !*size || *end1 || *end2 || a > INT64_MAX || !n || n > INT64_MAX - a) return NULL;
    mng_base_range *range = calloc(1, sizeof(*range));
    if (!range) return NULL;
    range->file = fopen(path, "rb"); range->start = (off_t)a; range->size = (off_t)n;
    if (!range->file) { free(range); return NULL; }
    if (fseeko(range->file, 0, SEEK_END) || ftello(range->file) < (off_t)(a + n)) { mng_base_close(range); return NULL; }
    FILE *view = funopen(range, mng_base_read, NULL, mng_base_seek, mng_base_close);
    if (!view) mng_base_close(range);
    return view;
}
'''
        marker = 'int main(int argc, char **argv)'
        if text.count(marker) != 1 or text.count('nca_ctx.tool_ctx->base_file = fopen(optarg, "rb")') != 2:
            raise BuildError('pinned hactool bounded base-file hook changed')
        text = text.replace(marker, helper + '\n' + marker)
        text = text.replace('nca_ctx.tool_ctx->base_file = fopen(optarg, "rb")',
                            'nca_ctx.tool_ctx->base_file = mng_open_base(optarg)')
        main.write_text(text)
    # Some newer cumulative update NCAs leave sector-aligned padding after the
    # BKTR subsection table. The relocation and subsection tables still have to
    # be contiguous and remain inside the section; IVFC verification below
    # validates the reconstructed RomFS data. Keep hactool's strict default for
    # the table boundary and permit only this unused trailing section space.
    nca = source/'nca.c'
    text = nca.read_text()
    old = ('sb->relocation_header.offset + sb->relocation_header.size != sb->subsection_header.offset ||\n'
           '            sb->subsection_header.offset + sb->subsection_header.size != ctx->size')
    new = ('sb->relocation_header.offset + sb->relocation_header.size != sb->subsection_header.offset ||\n'
           '            sb->subsection_header.offset + sb->subsection_header.size > ctx->size ||\n'
           '            (ctx->size - (sb->subsection_header.offset + sb->subsection_header.size)) % 0x200 != 0')
    if new not in text:
        if text.count(old) != 1:
            raise BuildError('pinned hactool BKTR layout check changed')
        nca.write_text(text.replace(old, new))
    text = nca.read_text()
    old = 'if (ctx->bktr_ctx.subsection_block->total_size != sb->subsection_header.offset) {'
    new = ('if (ctx->bktr_ctx.subsection_block->total_size != sb->subsection_header.offset &&\n'
           '            (sb->relocation_header.size > sb->subsection_header.offset ||\n'
           '             ctx->bktr_ctx.subsection_block->total_size != sb->subsection_header.offset - sb->relocation_header.size)) {')
    if new not in text:
        if text.count(old) != 1:
            raise BuildError('pinned hactool BKTR total-size check changed')
        nca.write_text(text.replace(old, new))
    config = source/'config.mk'
    if not config.exists():
        shutil.copy2(source/'config.mk.template', config)
    run(['make', '-j2', 'CC=clang'], logs/'hactool-build.log', cwd=source)
    return source/'hactool'

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    work = args.work.expanduser().resolve()
    print(build_hactool(work, work / 'logs'))

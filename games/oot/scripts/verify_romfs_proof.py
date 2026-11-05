#!/usr/bin/env python3
"""Exercise production RomFS verification with a read-only image and external proof.

Uses a tiny generated image, never game files. Tests a full check, cached reuse,
forced recheck, and rejection of changed image bytes, for single/split images.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    source = (Path(__file__).resolve().parents[1]/'runtime/romfs.c').read_text()
    # Compile the production verification section, independent of the guest loader.
    source = source[:source.index('static uint64_t lower(const char *name);')]
    source = '\n'.join(line for line in source.splitlines()
                       if not line.startswith('#include "'))
    payload = b'generated RomFS proof fixture\n' * 17
    digest = hashlib.sha256(payload).hexdigest()
    header = f'''#include <stdint.h>
#include <pthread.h>
#define ROMFS_IMAGE_SHA256 "{digest}"
#define ROMFS_IMAGE_SIZE {len(payload)}u
void runtime_fail(const char *message);
void runtime_boot_progress(const char *stage, uint64_t done, uint64_t total);
void runtime_boot_status(const char *stage, const char *detail);
'''
    harness = r'''
void runtime_fail(const char *message) { fprintf(stderr,"FAIL: %s\n",message); exit(7); }
void runtime_boot_progress(const char *stage,uint64_t done,uint64_t total) {
 (void)stage;(void)done;(void)total;
}
void runtime_boot_status(const char *stage,const char *detail) {
 (void)stage;(void)detail;
}
int main(int argc,char **argv) {
 if(argc!=3)return 2;
 if(!strcmp(argv[1],"split")){open_chunks(argv[2]);verify_chunks(argv[2]);}
 else {int fd=open(argv[2],O_RDONLY);if(fd<0)return 3;verify_image(fd,"image");close(fd);}
 return 0;
}
'''
    results = []
    with tempfile.TemporaryDirectory(prefix='totk-romfs-proof-') as temporary:
        root = Path(temporary)
        (root/'probe.c').write_text(header+source+harness)
        subprocess.run(['xcrun', 'clang', '-std=c11', '-Wno-deprecated-declarations',
                        str(root/'probe.c'), '-o', str(root/'probe')], check=True)
        for mode in ('single', 'split'):
            image = root/'image'
            image.mkdir()
            proof = root/'proof'
            proof.mkdir()
            if mode == 'single':
                target = image/'romfs.bin'
                target.write_bytes(payload)
                command = [str(root/'probe'), mode, str(target)]
                record = proof/'.romfs-verified-v1'
            else:
                for i, start in enumerate(range(0, len(payload), 200)):
                    (image/f'romfs.bin.{i:03d}').write_bytes(payload[start:start+200])
                target = image/'romfs.bin.000'
                command = [str(root/'probe'), mode, str(image)]
                record = proof/'.romfs-verified-parts-v1'
            expected_names = sorted(p.name for p in image.iterdir())
            for file in image.iterdir():
                file.chmod(0o444)
            image.chmod(0o555)
            env = os.environ | {'TOTK_ROMFS_PROOF_DIR': str(proof)}
            env.pop('TOTK_ROMFS_VERIFY', None)

            def run(extra=None):
                return subprocess.run(command, env=env | (extra or {}), cwd=root,
                                      capture_output=True, text=True)

            try:
                first = run()
                assert first.returncode == 0, first.stderr
                assert record.is_file(), 'proof must be in external writable folder'
                assert sorted(p.name for p in image.iterdir()) == expected_names
                before = record.read_bytes()
                reuse = run()
                assert reuse.returncode == 0 and 'reused full SHA256' in reuse.stderr, reuse.stderr
                assert record.read_bytes() == before
                forced = run({'TOTK_ROMFS_VERIFY': 'full'})
                assert forced.returncode == 0 and '[forced]' in forced.stderr, forced.stderr
                target.chmod(0o644)
                data = target.read_bytes()
                target.write_bytes(bytes([data[0] ^ 1])+data[1:])
                target.chmod(0o444)
                changed = run()
                assert changed.returncode == 7 and 'RomFS integrity mismatch' in changed.stderr
                assert record.read_bytes() == before, 'failed check must not replace valid proof'
                assert sorted(p.name for p in image.iterdir()) == expected_names
                results.append({'mode': mode, 'full': 'pass', 'reuse': 'pass',
                                'forced': 'pass', 'changed_bytes_rejected': True,
                                'image_directory_unchanged': True})
            finally:
                image.chmod(0o755)
                for file in image.iterdir():
                    file.chmod(0o644)
            import shutil
            shutil.rmtree(image)
            shutil.rmtree(proof)
    print(json.dumps({'status': 'pass', 'cases': results}, indent=2))


if __name__ == '__main__':
    main()

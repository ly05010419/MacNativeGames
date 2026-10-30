"""Game-data-free regressions for DLC identities, immutable images and mount semantics."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from games.acnh.tools import prepare_dlc

GAME = Path(__file__).resolve().parents[1]


class DlcAssetTests(unittest.TestCase):
    def test_verified_bytes_determine_file_ranges_and_corruption_is_rejected(self):
        name, payload = b'value.bin', b'synthetic data'
        image = (struct.pack('<10Q', 80, 80, 0, 80, 24, 104, 0, 104, 32 + len(name), 136 + len(name))
                 + struct.pack('<6I', 0xffffffff, 0xffffffff, 0xffffffff, 0, 0xffffffff, 0)
                 + struct.pack('<IIQQII', 0, 0xffffffff, 0, len(payload), 0xffffffff, len(name))
                 + name + payload)
        row = dict(title_id='01006F800232712C', index=300, romfs_size=len(image),
                   romfs_sha256=hashlib.sha256(image).hexdigest(), file_count=1)
        pins = dict(title_id='01006F8002326000', entries=[row])
        with tempfile.TemporaryDirectory() as temporary, patch.object(prepare_dlc, 'pins', return_value=pins):
            root = Path(temporary)
            manifest = root / 'manifest.json'
            manifest.write_text(json.dumps(dict(schema=1, **pins)))
            binary = root / f'{row["title_id"]}.romfs'
            binary.write_bytes(image)
            _, files = prepare_dlc.verify_assets(root)
            self.assertEqual(files, [[dict(path='value.bin', offset=136 + len(name), size=len(payload))]])
            header, digest = prepare_dlc.config_header(root)
            self.assertIn(digest, header)
            binary.write_bytes(image[:-1] + b'!')
            with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
                prepare_dlc.verify_assets(root)
            binary.write_bytes(image)
            data = json.loads(manifest.read_text()); data['entries'][0]['index'] = 1000
            manifest.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, 'manifest differs'):
                prepare_dlc.verify_assets(root)

    def test_any_pinned_subset_in_pinned_order(self):
        rows = [dict(title_id=f'01006F80023271{i:02X}', index=i, romfs_size=0, romfs_sha256='', file_count=0)
                for i in range(3)]
        pins = dict(title_id='01006F8002326000', entries=rows)
        with tempfile.TemporaryDirectory() as temporary, patch.object(prepare_dlc, 'pins', return_value=pins), \
                patch.object(prepare_dlc, 'verified_file'), patch.object(prepare_dlc, 'index_romfs', return_value=[]):
            root = Path(temporary)
            for row in rows:
                (root / f'{row["title_id"]}.romfs').write_bytes(b'')
            for chosen, ok in (([rows[2]], True), ([rows[0], rows[2]], True), ([rows[2], rows[0]], False),
                               ([], False), ([rows[1], rows[1]], False), ([dict(rows[1], index=9)], False)):
                with self.subTest(chosen=[row['index'] for row in chosen]):
                    (root / 'manifest.json').write_text(json.dumps(dict(schema=1, title_id=pins['title_id'], entries=chosen)))
                    if ok:
                        self.assertEqual(prepare_dlc.verify_assets(root)[0]['entries'], chosen)
                    else:
                        with self.assertRaisesRegex(ValueError, 'manifest differs'):
                            prepare_dlc.verify_assets(root)

class DlcRuntimeTests(unittest.TestCase):
    def test_mounts_bounds_missing_images_and_mutation(self):
        source = r'''
#include "runtime.h"
#include "dlc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
_Noreturn void runtime_fail(const char *message) { fprintf(stderr,"%s\n",message); exit(77); }
void *runtime_fs_host_override(const char *name) { (void)name; return NULL; }
void *runtime_override(const char *name) { (void)name; return NULL; }
static int children;
static void child(void *ctx,const char *name,int dir,int64_t size) {
    (void)ctx; if (strcmp(name,"value") || dir || size!=3) exit(20); children++;
}
int main(int argc,char **argv) {
    if (argc!=2) return 1;
    if (runtime_dlc_count()!=2) return 2;
    int ids[3]={-1,-1,99};
    if (runtime_dlc_list(ids,0,2)!=2 || ids[0]!=300 || ids[1]!=1000 || ids[2]!=99) return 3;
    if (runtime_dlc_list(ids,1,1)!=1 || ids[0]!=1000 || runtime_dlc_list(ids,3,1) || runtime_dlc_list(NULL,0,0)) return 4;
    if (!strcmp(argv[1],"invalid")) { runtime_dlc_list(NULL,0,1); return 21; }
    if (!strcmp(argv[1],"mutate")) {
        char path[4096]; snprintf(path,sizeof path,"%s/first.romfs",getenv("ACNH_DLC_DIR"));
        FILE *f=fopen(path,"ab"); if(!f) return 22; fputc('x',f); fclose(f);
    }
    if (runtime_dlc_mount("one",300) || runtime_dlc_mount("two",1000)) return 5;
    if (!runtime_dlc_mount("one",300) || !runtime_dlc_mount("absent",9) || !runtime_dlc_mount("../bad",300)) return 6;
    if (!runtime_dlc_owns("one:/dir/value") || runtime_dlc_owns("other:/dir/value")) return 7;
    int fd=-1,type; uint64_t base,size; char data[3];
    if (runtime_dlc_open(&fd,&base,&size,"one:/dir/value",1) || base!=1 || size!=3) return 8;
    if (pread(fd,data,3,(off_t)base)!=3 || memcmp(data,"abc",3)) return 9; close(fd);
    if (runtime_dlc_open(&fd,&base,&size,"two:/value",1) || base!=2 || size!=2) return 10; close(fd);
    if (!runtime_dlc_open(&fd,&base,&size,"one:/dir/value",2) || !runtime_dlc_open(&fd,&base,&size,"one:/../escape",1)) return 11;
    if (runtime_dlc_entry_type(&type,"one:/") || type || runtime_dlc_entry_type(&type,"one:/dir/") || type) return 12;
    if (runtime_dlc_entry_type(&type,"one:/dir/value") || type!=1 || !runtime_dlc_entry_type(&type,"one:/absent")) return 13;
    if (runtime_dlc_directory("one:/dir",3,child,NULL) || children!=1) return 14;
    if (!runtime_dlc_unmount("one") || runtime_dlc_owns("one:/") || runtime_dlc_unmount("one")) return 15;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'first.romfs').write_bytes(b'0abc9')
            (root / 'second.romfs').write_bytes(b'00yz9')
            sha1 = hashlib.sha256(b'0abc9').hexdigest(); sha2 = hashlib.sha256(b'00yz9').hexdigest()
            (root / 'dlc_config.h').write_text(
                '#define ACNH_DLC_COUNT 2\n#define ACNH_DLC_DEFAULT_DIR "unused"\n#define ACNH_DLC_MARKER "test"\n'
                'static const DlcFile first[]={{"dir/value",1,3}},second[]={{"value",2,2}};\n'
                f'static const DlcImage dlc_images[]={{{{300,"first",5,"{sha1}",first,1}},{{1000,"second",5,"{sha2}",second,1}}}};\n')
            probe, binary = root / 'probe.c', root / 'probe'
            probe.write_text(source)
            build = subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-deprecated-declarations', '-I', str(root), '-I', str(GAME / 'runtime'),
                str(probe), str(GAME / 'runtime/dlc.c'), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            env = dict(os.environ, ACNH_DLC_DIR=str(root))
            for mode, expected, message in [('normal', 0, ''), ('invalid', 77, 'range/buffer'),
                                            ('mutate', 77, 'changed after verification')]:
                run = subprocess.run([str(binary), mode], env=env, capture_output=True, text=True)
                self.assertEqual(run.returncode, expected, run.stderr)
                self.assertIn(message, run.stderr)
            # A full hash is recorded, reused while the image is unchanged, and redone after an edit.
            (root / 'first.romfs').write_bytes(b'0abc9'); (root / 'proofs').mkdir()
            env['TOTK_ROMFS_PROOF_DIR'] = str(root / 'proofs')
            for reused in (False, True):
                run = subprocess.run([str(binary), 'normal'], env=env, capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertEqual('reused full SHA256' in run.stderr, reused, run.stderr)
            forced = subprocess.run([str(binary), 'normal'], env=dict(env, TOTK_ROMFS_VERIFY='full'), capture_output=True, text=True)
            self.assertEqual(forced.returncode, 0, forced.stderr); self.assertNotIn('reused', forced.stderr)
            (root / 'first.romfs').write_bytes(b'0abc8')
            corrupt = subprocess.run([str(binary), 'normal'], env=env, capture_output=True, text=True)
            self.assertEqual(corrupt.returncode, 77)
            self.assertIn('SHA-256 mismatch', corrupt.stderr)
            (root / 'first.romfs').unlink()
            missing = subprocess.run([str(binary), 'normal'], env=env, capture_output=True, text=True)
            self.assertEqual(missing.returncode, 77)
            self.assertIn('image missing', missing.stderr)

    def test_build_without_dlc_keeps_an_empty_list(self):
        source = r'''
#include "runtime.h"
#include "dlc.h"
#include <stdlib.h>
_Noreturn void runtime_fail(const char *message) { (void)message; exit(77); }
void *runtime_fs_host_override(const char *name) { (void)name; return NULL; }
void *runtime_override(const char *name) { (void)name; return NULL; }
int main(void) {
    int ids[2]={99,98};
    return runtime_dlc_count() || runtime_dlc_list(ids,0,2) || ids[0]!=99 || ids[1]!=98
        || runtime_dlc_has(300) || runtime_dlc_owns("aoc:/") || !runtime_dlc_mount("aoc",300);
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); probe = root / 'empty.c'; binary = root / 'empty'
            probe.write_text(source)
            (root / 'dlc_config.h').write_text('#define ACNH_DLC_COUNT 0\n#define ACNH_DLC_MARKER "MNG_ACNH_DLC:none"\n')
            build = subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-deprecated-declarations', '-I', str(root), '-I', str(GAME / 'runtime'),
                str(probe), str(GAME / 'runtime/dlc.c'), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            subprocess.run([str(binary)], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()

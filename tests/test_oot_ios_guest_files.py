"""Ocarina of Time on iOS keeps the game's memory in files, as nxnative does (user 2026-11-10: "照 nxnative 做，只限 iOS").

On an iPhone 14 the guest regions held 3,357 MB of a 4,095 MB memory limit and the app was ended (res-mem-4); a smaller pool
crashes the game. So on iOS the stack, heap and alias block is mapped from unlinked sparse 1 GB files, what the game gives back
is punched out, and a thread stops the game before the disk fills. The Mac keeps anonymous memory. These check the source, as
the iOS build is not run here.
"""
import re
import unittest
from pathlib import Path

SERVICES = Path(__file__).resolve().parents[1] / 'games/oot/runtime/services.c'


def function(source, name):
    """The first definition of name (a line with its body's brace, not a prototype) up to its closing brace."""
    for found in re.finditer(re.escape(name) + r'\(', source):
        start = source.rindex('\n', 0, found.start()) + 1
        line = source[start:source.index('\n', found.start())]
        if '{' in line:
            return source[start:source.index('\n}\n', start)]
    raise AssertionError(f'no definition of {name}')


class GuestFiles(unittest.TestCase):
    source = SERVICES.read_text()

    def ios_block(self):
        start = self.source.index('#if TARGET_OS_IPHONE\n#include <os/proc.h>')
        middle = self.source.index('#else\nstatic int guest_files_init(void){return 0;}', start)
        end = self.source.index('#endif', middle)
        return self.source[start:middle], self.source[middle:end]

    def test_only_ios_puts_the_guest_in_files(self):
        phone, mac = self.ios_block()
        self.assertIn('static int guest_files_init(void){', phone)
        self.assertIn('static int gf_alias_release(uint64_t page){(void)page;return 0;}', mac)
        self.assertIn('static void gf_alias_zero(void *a){memset(a,0,4096);}', mac)
        self.assertIn('static void gf_heap_resized(uint64_t was,uint64_t now){(void)was;(void)now;}', mac)

    def test_on_when_the_pool_does_not_fit_the_room(self):
        body = function(self.source, 'static int guest_files_init')
        self.assertIn('os_proc_available_memory()', body)
        self.assertIn('getenv("OOT_GUEST_FILE")', body)
        self.assertRegex(body, r'want=e&&\*e\?atoi\(e\)>0:room&&APP_POOL>room')

    def test_files_are_unlinked_sparse_shared_class_c_and_1_gb(self):
        body = function(self.source, 'static int guest_files_init')
        self.assertIn('gf_chunk=1ULL<<30', body)
        self.assertIn('open_dprotected_np(path,fl,3,0,0600)', body)
        self.assertLess(body.index('gf_tab[made++]=fd'), body.index('if(unlink(path))'))
        self.assertIn('ftruncate(gf_tab[k]', body)
        self.assertIn('MAP_SHARED|MAP_FIXED,gf_tab[k],0', body)
        # a failure leaves the block anonymous PROT_NONE again, as reserved
        self.assertIn('MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)!=(void *)low_space', body)

    def test_memory_init_tries_the_files_before_the_old_heap_file(self):
        body = function(self.source, 'static void memory_init')
        self.assertIn('heap=low_space+HEAP_OFF;if(!guest_files_init())heap_file_backing();', body)

    def test_alias_release_keeps_the_file_mapping(self):
        body = function(self.source, 'static uint64_t alias_map')
        release = body.index('if(gf_alias_release(page))continue;')
        self.assertLess(release, body.index('MAP_FIXED|MAP_PRIVATE|MAP_ANON'))
        opened = body.index('gf_alias_open(low,high);')
        self.assertLess(body.index('commit physical alias pages'), opened)
        self.assertIn('gf_alias_zero((void *)(addr+(uint64_t)i*4096))', body)
        released = function(self.source, 'static int gf_alias_release')
        self.assertIn('PROT_NONE', released)
        self.assertNotIn('alias_stale', released)   # the bytes stay until punched: the pages are zeroed when mapped again

    def test_sweep_punches_only_what_stayed_given_back_a_whole_interval(self):
        body = function(self.source, 'static void gf_sweep')
        self.assertIn('pthread_mutex_lock(&memory_lock)', body)
        self.assertIn('alias_aged[w]', body)
        self.assertIn('alias_rel[w]&=~alias_aged[w];alias_aged[w]=alias_rel[w];', body)
        opened = function(self.source, 'static void gf_alias_open')
        self.assertIn('alias_rel[h>>6]&=m;alias_aged[h>>6]&=m;', opened)

    def test_heap_shrink_is_punched_and_regrowth_reads_zero(self):
        self.assertIn('gf_heap_resized(heap_size,size);heap_size=size;', self.source)
        body = function(self.source, 'static void gf_heap_resized')
        self.assertIn('gf_punch((uintptr_t)heap+now,(uintptr_t)heap+was)', body)
        self.assertIn('gf_zero(heap+was,', body)

    def test_zeroing_leaves_holes_clean(self):
        body = function(self.source, 'static void gf_zero')
        self.assertIn('if(any)memset(a,0,4096);', body)

    def test_the_game_stops_before_the_disk_fills(self):
        body = function(self.source, 'static void *gf_thread')
        self.assertIn('_exit(87)', body)
        self.assertIn('gf_free_important()', body)
        self.assertIn('MINCORE_MODIFIED', function(self.source, 'static double gf_dirty'))
        self.assertRegex(self.source, r'gf_min_free=2ULL<<30')


if __name__ == '__main__':
    unittest.main()

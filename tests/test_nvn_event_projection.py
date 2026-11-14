#!/usr/bin/env python3
"""Compile the renderer's actual event-projection fragment; no Metal/GPU work.

Do not run compilation during a timed game. An unchanged 512-entry renderer must
fail at the 513th distinct event word, after its ordinary semantics checks pass.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest


PRELUDE = r"""
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t g_fence, g_done;
static _Atomic uint64_t g_cur_frame;
#define MDIE(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); abort(); } while (0)
"""

FIXTURE = r"""
static unsigned checked;
static void require(int ok, const char *message) {
    checked++;
    if (!ok) { fprintf(stderr, "FAIL after %u checks: %s\n", checked, message); exit(1); }
}
static void ordinary_semantics(volatile uint32_t *word) {
    g_fence = 100; g_done = 99; atomic_store(&g_cur_frame, 7);
    *word = 0;
    require(!mtl_qo_event_ready(word, 0, 10), "unseen event cannot satisfy EQUAL");
    qwait_signal(word, 0, 10);
    require(*word == 0, "projection must not modify CPU event bytes");
    require(mtl_qo_event_ready(word, 0, 10), "pending SET projection satisfies EQUAL");
    require(mtl_qo_event_ready(word, 1, 9), "pending projection satisfies GEQUAL");
    require(!mtl_qo_event_ready(word, 1, 11), "future GEQUAL target stays blocked");
    require(mtl_qo_event_gpu_projected(word), "incomplete projected fence remains pending");
    qwait_signal(word, 1, 3);
    require(mtl_qo_event_ready(word, 0, 13), "ADD uses preceding pending projection");
    require(mtl_qo_event_ready(word, 0, 10), "EQUAL history survives a later signal");
    for (unsigned i = 0; i < 8; i++) qwait_signal(word, 1, 1);
    require(mtl_qo_event_ready(word, 0, 21), "incremented current projection is retained");
    require(mtl_qo_event_ready(word, 0, 14), "oldest of eight history values survives");
    require(!mtl_qo_event_ready(word, 0, 13), "history older than eight signals expires");
    g_done = 101;
    require(!mtl_qo_event_gpu_projected(word), "completion stops pending-GPU classification");
    *word = 100; g_fence = 101;
    qwait_signal(word, 1, 2);
    require(mtl_qo_event_ready(word, 0, 102), "ADD after completion uses CPU-visible bytes");
    *word = 200; mtl_qo_cpu_signal(word);
    require(mtl_qo_event_ready(word, 0, 200), "CPU signal remains authoritative");
    require(!mtl_qo_event_ready(word, 0, 21), "CPU reset clears stale EQUAL history");
    require(!mtl_qo_event_gpu_projected(word), "CPU reset clears projected fence");
    require(qwait_ready(UINT32_C(3), 1, UINT32_C(0xfffffff0)), "GEQUAL handles counter wrap");
    require(!qwait_ready(UINT32_C(0xffffff00), 1, UINT32_C(3)), "GEQUAL rejects a future wrapped target");
    g_fence = 200; g_done = 100;
    qwait_signal(word, 0, 300); qwait_signal(word, 1, 1);
    require(mtl_qo_event_ready(word, 0, 300), "history restored for later growth check");
}
enum { THREADS = 4, WORDS_PER_THREAD = 512 };
typedef struct { volatile uint32_t *words; unsigned failures; } worker_input;
static pthread_mutex_t start_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cv = PTHREAD_COND_INITIALIZER;
static unsigned workers_ready;
static int workers_go;
static void *worker(void *arg) {
    worker_input *w = arg;
    pthread_mutex_lock(&start_mu);
    workers_ready++; pthread_cond_broadcast(&start_cv);
    while (!workers_go) pthread_cond_wait(&start_cv, &start_mu);
    pthread_mutex_unlock(&start_mu);
    /* Every thread owns distinct event words. The table and its resize are the
       shared state; no concurrent CPU writes to one event are assumed safe. */
    for (unsigned i = 0; i < WORDS_PER_THREAD; i++) {
        volatile uint32_t *v = &w->words[i];
        qwait_signal(v, 0, 17);
        if (!mtl_qo_event_ready(v, 0, 17)) w->failures++;
        qwait_signal(v, 1, 3);
        if (!mtl_qo_event_ready(v, 0, 20)) w->failures++;
        *v = 5; mtl_qo_cpu_signal(v);
        if (mtl_qo_event_ready(v, 0, 20) || !mtl_qo_event_ready(v, 0, 5)) w->failures++;
        if (!(i & 15)) sched_yield();
    }
    return NULL;
}
int main(void) {
    static volatile uint32_t words[769];
    ordinary_semantics(&words[0]);
    qwait_ev saved;
    qwait_ev *first = qwait_find(&words[0], 0);
    require(first != NULL, "existing event must be present before growth");
    memcpy(&saved, first, sizeof saved);
    for (unsigned i = 1; i < 769; i++) {
        uint32_t target = 1000 + i;
        qwait_signal(&words[i], 0, target);
        checked++;
        if (!mtl_qo_event_ready(&words[i], 0, target)) {
            fprintf(stderr, "FAIL after %u checks: lost projection at distinct event %u; CPU=%u target=%u\n",
                    checked, i + 1, words[i], target);
            return 1;
        }
    }
    first = qwait_find(&words[0], 0);
    require(first && !memcmp(first, &saved, sizeof saved), "growth preserves all bytes of existing projection/history/fence");
    for (unsigned i = 1; i < 769; i++)
        require(mtl_qo_event_ready(&words[i], 0, 1000 + i), "earlier distinct event survives later growth");
    require(mtl_qo_event_ready(&words[0], 0, 300), "earlier EQUAL history survives growth");
    require(mtl_qo_event_ready(&words[0], 0, 301), "earlier current projection survives growth");
    pthread_t threads[THREADS]; worker_input input[THREADS] = {0};
    for (unsigned i = 0; i < THREADS; i++) {
        input[i].words = calloc(WORDS_PER_THREAD, sizeof(uint32_t));
        require(input[i].words != NULL, "worker event allocation");
        require(!pthread_create(&threads[i], NULL, worker, &input[i]), "worker creation");
    }
    pthread_mutex_lock(&start_mu);
    while (workers_ready < THREADS) pthread_cond_wait(&start_cv, &start_mu);
    workers_go = 1; pthread_cond_broadcast(&start_cv);
    pthread_mutex_unlock(&start_mu);
    for (unsigned i = 0; i < THREADS; i++) {
        require(!pthread_join(threads[i], NULL), "worker join");
        require(!input[i].failures, "concurrent distinct-address projection/CPU reset survives growth");
    }
    /* Keep storage live to process exit: the production table is address-keyed.
       Event or pool reclamation semantics are deliberately outside this test. */
    printf("PASS %u assertions; 769 sequential + %u concurrent distinct event words\n", checked, THREADS * WORDS_PER_THREAD);
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--extract-only', action='store_true')
    parser.add_argument('--sanitize', choices=['address', 'thread'])
    args = parser.parse_args()
    raw = args.source.read_bytes()
    text = raw.decode()
    start = text.index('uint64_t g_qwait_queue;')
    end = text.index('static void qwait_check(', start)
    fragment = text[start:end]
    args.out.mkdir(parents=True, exist_ok=True)
    generated = args.out / 'qwait_actual_source.c'
    generated.write_text(PRELUDE + fragment + FIXTURE)
    executable = args.out / 'qwait_actual_source'
    command = ['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-Wno-unused-variable', '-pthread']
    if args.sanitize:
        command += ['-fsanitize=' + args.sanitize, '-fno-omit-frame-pointer']
    command += [str(generated), '-o', str(executable)]
    metadata = dict(source=str(args.source), source_sha256=hashlib.sha256(raw).hexdigest(),
                    fragment_sha256=hashlib.sha256(fragment.encode()).hexdigest(),
                    fixture_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    compiler_command=command, extracted_only=args.extract_only)
    (args.out / 'provenance.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata), flush=True)
    if args.extract_only:
        return 0
    subprocess.run(command, check=True)
    return subprocess.run([str(executable)]).returncode


class EventProjectionTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang'), 'clang is required')
    def test_capacity_history_and_concurrent_cpu_reset(self):
        source = (Path(__file__).resolve().parents[1] /
                  'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m')
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [sys.executable, str(Path(__file__).resolve()), '--source', str(source),
                 '--out', directory], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('concurrent distinct event words', result.stdout)


if __name__ == '__main__':
    if '--source' in sys.argv or any(a.startswith('--source=') for a in sys.argv):
        raise SystemExit(main())
    unittest.main()

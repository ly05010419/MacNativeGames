/* 崩溃报告闸门（思路取自 nxnative d47f899 的 crash.c，MIT，github.com/seedds/nxnative；代码是本项目自己的）。
   只让第一个崩溃的线程打印现场：几条线程一起崩时（作业线程常一起撞上同一份坏数据）各自的现场原先交错成一团。
   后来的线程停在 pause() 里，等第一个线程打完后 _exit 结束进程；第一个线程自己又进来（打印时又出了别的信号）就立即退出。
   打印卡住时（读到挂起线程持有的东西等）由 seconds 秒后的 SIGALRM 收尾：写一行后以 128 + 原信号退出，进程不会挂着不走。
   只在确定要报告崩溃之后调用：能恢复的故障（runtime_fault_guard 守护的读、图形层的脏页故障、模拟的指令）必须在它之前处理完返回，
   否则一个线程在打印时，别的线程的可恢复故障会被当成崩溃停住。只用异步信号安全的调用。
   Crash report gate (idea from nxnative d47f899's crash.c, MIT, github.com/seedds/nxnative; the code is this project's own).
   Only the first crashing thread prints its state: when several threads crash together (job threads often run into the same bad
   data), their dumps used to interleave. Later threads stop in pause() until the first one finishes and ends the process with
   _exit; the first thread entering again (another signal while it prints) exits at once. A dump that stalls (reading something a
   suspended thread holds, say) is ended by a SIGALRM after `seconds`: one line, then exit with 128 + the original signal, so the
   process never hangs. Call it only once a crash is certain: recoverable faults (reads guarded by runtime_fault_guard, the graphics
   layer's dirty-page faults, emulated instructions) must be handled and returned from before it, or a recoverable fault on another
   thread would be stopped as a crash while one thread prints. Uses only async-signal-safe calls. */
#pragma once
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>

static uintptr_t crash_gate_owner;   /* pthread_self() of the thread printing the crash, 0 = none */
static int crash_gate_signal;
static void crash_gate_alarm(int sig) {
    (void)sig;
    static const char note[] = "[totk] crash report stalled: exiting\n";
    ssize_t w = write(2, note, sizeof note - 1); (void)w;
    _exit(128 + __atomic_load_n(&crash_gate_signal, __ATOMIC_SEQ_CST));
}
/* Returns only in the first crashing thread; every other caller waits for the process to end or exits it. */
static void crash_gate(int sig, unsigned seconds) {
    uintptr_t me = (uintptr_t)pthread_self(), none = 0;
    if (__atomic_compare_exchange_n(&crash_gate_owner, &none, me, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&crash_gate_signal, sig, __ATOMIC_SEQ_CST);
        if (seconds) {
            struct sigaction a = {0}; a.sa_handler = crash_gate_alarm; sigemptyset(&a.sa_mask);
            sigaction(SIGALRM, &a, NULL);
            sigset_t s; sigemptyset(&s); sigaddset(&s, SIGALRM); pthread_sigmask(SIG_UNBLOCK, &s, NULL);
            alarm(seconds);
        }
        return;
    }
    if (none == me) _exit(128 + sig);   /* the printing thread itself, again */
    for (;;) pause();
}

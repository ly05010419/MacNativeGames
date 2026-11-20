# Experimental nested mutex priority donation

`TOTK_PI_CHAIN=1` enables a BOTW-only scheduler experiment. It is off by
default; normal launches and other games keep their existing behavior.
`TOTK_PI_STEP` with a nonzero value also retains its existing direct-only
diagnostic behavior.

## Reproduced problem

The current virtual-core priority query considers a mutex waiter's base
priority only. In a synthetic chain A (priority 44) → B (50) → C (60),
B inherits 44, but C only inherits B's base priority 50. Unrelated runnable
work at priority 45 is therefore selected before C, even though C must run
before A can make progress.

The opt-in query follows the current mutex owners and donates each waiting
thread's base priority along the full chain. It uses live generation-checked
thread handles, no cached owner pointers, no allocation and no recursion.
Cycle detection bounds a query even when the guest has a mutex deadlock;
it does not resolve such a deadlock. User-space handoffs and removed waiters
are reflected at the next query. Condition/address/event waits do not create
mutex-owner edges.

This changes the effective-priority query used by virtual-core selection.
It is not a complete rewrite of mutex waiter ordering or host scheduling.
The chain semantics are informed by the public
[Atmosphère thread implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_thread.cpp)
(`RestorePriority` follows the next lock owner); this implementation and its
tests are original code, with no source copied from that project.

## Validation and limits

Native tests include the production scheduler and execute its real priority
query and core grant. They cover direct/nested/128-thread chains, multiple
donors, list ordering, handoffs, waiter removal, cycles, stale generations,
wrong-type owners, non-mutex waits and unchanged defaults/diagnostic steps.
Another 512 generated graphs are checked against an independent fixed-point
priority reference, under UBSan.

A local Mac M4 Pro loop microbenchmark, with a compiler memory barrier between
queries, measured roughly 2–3 ns per empty query,
about 12 ns for a direct-owner case, and 122 ns with the opt-in versus 43 ns
without it for a 39-edge chain. Those two long-chain queries produce different
priorities: the difference is added work for propagation, not a speedup.
These are host measurements, not iPad CPU costs or whole-frame results.

The iPad previously exhibited unexplained 2–6 second in-game stalls, but no
captured stall has yet been attributed to a nested ownership chain. Hardware validation of this candidate remains pending. This experiment is not a
confirmed fix for those stalls and does not establish stable 60 FPS.

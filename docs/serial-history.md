# Long-session metadata and shader-translation stalls

The 2026-10-19 M3 / 24 GiB, 1.0.0 session reached roughly 2h52m. Its dead-serial
and GPU-owned history sets had 24.37M and 16.70M entries by frame 781800. Their
hash-table arrays alone were approximately 1 GiB by capacity calculation.
Expansions near frames 426689 and 616322 coincided with 234ms and 185ms
non-compilation frames and large footprint steps. This is correlation rather
than a sampled call-stack attribution of those exact frames.

## Exact paged serial history

Replace those two append-only hash sets with the production
`totk_serial_set.h`: a sparse radix index into 65,536-bit pages, with a small
page cache. Each stored fact uses one bit in its allocated page. A new page
allocates at most five index nodes plus an 8 KiB leaf after the root exists;
no operation reinserts the previously observed serials.

Membership, duplicate insertion and the full uint64 serial domain are preserved.
The existing `g_smu` still protects all accesses, including the page cache.
Nothing clears a historical fact or changes GPU fences, texture-pool following,
alias ownership, descriptor lifetime or recording pins. The old history cannot
be discarded just because its recording copy disappeared: pool slots can still
need it. The bitmap avoids that lifetime risk while reducing the cost of
retaining history. Storage still grows with occupied serial ranges; this is
compression and removal of global rehashes, not a claim of zero growth or a new
lifetime garbage collector. Allocation failure remains fatal as before.

The memory log includes actual history bytes and pages; a separate 3,000-frame
line continues after the existing detailed memory log reaches its 400-line cap.

## Validation

- CPU regression covers full-width serials, cached misses followed by insertion,
  duplicates, and two million retained facts. All old facts are checked after
  growth. Run `python3 -m unittest discover -s tests -p test_serial_set.py`.
- A synthetic two-million dense-serial comparison allocated 67,108,864 bytes in
  the former hash arrays versus 266,240 bytes in the paged structure. This is
  a data-structure comparison, not whole-process memory or an FPS benchmark.
- The local 1.0.0 packaged runtime also passed the texture-retirement GPU probe
  (35,021 checks) and a short save-copy scene/movement check. That build includes
  other local shader fixes, so this is supporting integration evidence, not an
  isolated performance comparison of this branch. No long-session plateau is
  claimed. UBSan passed; ASan could not initialize on the macOS 27.2 host.

This PR is independent of pipeline prediction PR #29 and asynchronous loading
PR #30. It changes only the representation of the two retained serial-history
sets and their memory diagnostics. It does not alter draw skipping, shader
translation, frame limits, resolution or interpolation settings.

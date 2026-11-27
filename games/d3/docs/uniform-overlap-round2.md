# Second-round D3 optimization — user-approved release

Stable baseline: `8919fae` (pushed). Its runtime was installed in the formal App;
`codesign --verify --deep --strict` passed. External release backup and SHA-256
manifest are in `D3_build/analysis/release-8919fae`. The data bundle and formal
profile were not changed.

The first candidate's passive CPU sample still showed overlap-query work inside
`upd_shadow`. The second candidate stops the reverse overlap scan once it finds
`g_upd_seq`, the global newest sequence: no earlier entry can exceed it. Queries
whose newest global update lies elsewhere still perform the original scan.
Equal-start updates are inserted after previous entries, making this shortcut
useful for D3's repeated per-object updates. No cache key or update lifetime is
changed. This is a smaller optimization than the baseline overlay rewrite.
The user reports more stable gameplay, mostly 110–120 FPS, and explicitly
approved updating the formal App, committing and pushing this version. This is
user-observed performance, not a controlled identical-scene benchmark.

Validation:
- 202,053 production insertion/query results match a brute-force reference under
  ASan/UBSan, including reset, same-address updates, touching boundaries and a
  global newest update outside the query.
- First-round overlay equivalence test still passes.
- Runtime build passes; GPU uniform-rename and snapshot probes pass again.

Second-round test logs: `D3_build/logs/crowded-round2`. Separate profile copied
from the first test profile, never merged into formal saves. Launch requested
120 Hz, no automated navigation. Formal App hash was checked after launching
and still matched the previous stable release during testing. Following user
approval, this version is promoted to the formal App with runtime hash and
code-signature verification. The previous `8919fae` release backup is retained;
formal saves are not replaced or merged from the test profile.

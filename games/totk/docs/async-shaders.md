# Async shader compilation on macOS

Open Settings with **1**, enable **Async shaders (next launch)**, and restart
the game. The choice is kept in `profile/async-shaders.txt`. An explicit
`TOTK_ASYNC_SHADERS=0` or `1` overrides it and locks the settings row.
The default remains off. Shader compilation can delay a first encounter; async
mode can reduce that wait by allowing eligible draws to be omitted temporarily.
Objects or effects can therefore be missing until compilation finishes. No
frame-time improvement is claimed without a measured game run.

The port already had background translation and pipeline compilation. This
change exposes the choice and improves its streaming compiler scheduling:

- Patched shader variants, deferred render pipelines, cached-variant prefetch
  and libraries produced by background translation share one bounded compiler
  pool. Its worker count comes from the Metal device's
  `maximumConcurrentCompilationTaskCount`, with a minimum of one and a fallback
  of two if the property is unavailable.
- The pool uses default QoS and an explicitly retained default-QoS underlying
  dispatch queue. Synchronous Metal compiler calls run on its workers; enqueueing
  a job does not wait for compilation. Background translator services also use
  default QoS. An urgent job that an unskippable draw needs retains its existing
  higher-priority path.
- Descriptor inputs are captured before submission. Existing cache locks,
  duplicate-job tracking and submit-thread pipeline adoption remain in force.
  A background translation publishes its ready generation only after its Metal
  library has finished. The submit thread never waits on the streaming pool.
- Existing binary archives and the startup warm-up remain in use. Compute,
  persistent-memory writes, required depth/shadow work and other non-skippable
  dependencies retain their waits. The TBDR rewrite and no-translator modes
  continue to disable async shader mode; iOS behavior is unchanged.

This follows Apple's current scheduling recommendations in
[Explore Metal 4 games](https://developer.apple.com/videos/play/wwdc2025/254/)
(pipeline loading, around 22 minutes), the device's
[concurrent-compilation limit](https://developer.apple.com/documentation/metal/mtldevice/maximumconcurrentcompilationtaskcount),
and the [Metal compilation workflow](https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api).
Apple permits synchronous compiler entry points on appropriately scheduled
background workers. This change retains the existing `MTLDevice` pipeline
descriptors and archive format; it does not migrate them to `MTL4Compiler`,
flexible pipeline specialization or `MTL4Archive`.

Validate scheduling with `python3 -m unittest discover -s tests -p 'test_async*.py'`.
The probe checks bounded concurrency, nonblocking submission, QoS configuration
and translation-publication ordering using synthetic jobs. Its separate Metal
smoke test builds a synthetic library and render pipeline when a Metal device
is available, and explicitly skips when none is exposed.

Before enabling by default, compare the same route, game version and settings
with cold and warm caches. Record complete frame-time p50/p95/p99, maximum and
over-budget frames; compare screenshots and check for transient missing draws,
compile errors and memory growth. Use Instruments Metal System Trace to inspect
compiler contention. Passing the scheduling probe alone is not a game-performance
or image-correctness result.

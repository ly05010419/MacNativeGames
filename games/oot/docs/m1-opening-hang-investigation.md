# M1 Max opening GPU hang investigation

Tested base: adc0e199f25982f1bc52dd33da6a25bf73676acb. Ocarina of Time v131072, M1 Max, macOS 27.0.1. This is an investigation and temporary diagnostic workaround, not a root-cause fix.

## Reproduction and isolation

The opening around the fairy entering Link's house can produce Metal GPU Hang feedback, followed by the existing protective abort (exit 134). It reproduces with the original movie path; the NDVE experiment in #2080 is not required to reproduce this particular hang.

A local default-off replay diagnostic associated two failing split submissions with compute shader prefix `93b8cf2a4840b5f2`. Replay records are not GPU execution order and a split can occur inside the current operation. Association alone does not establish causality.

Using the existing diagnostic environment variable `TOTK_SKIP_CS=93b8cf2a4840b5f2`, two independent opening runs had no recorded GPU Hang or fatal errors and closed normally. The first reached controllable gameplay inside Link's house. The second 270-second observation confirmed Link getting up after the fairy-at-bed scene; controllable gameplay was not separately verified in that run.

The user subsequently reported entering and playing normally with a local trial app combining this skip and the NDVE movie candidate. That combined trial is not an isolated test, and the user has not separately confirmed the opening-video artifact status.

## Limits and next work

Skipping this compute task discards its output and can change game state, visual results, workload or timing. Do not enable this globally or describe it as a correct rendering fix. Preserve the hang guard.

Sampled direct dispatches were 20 x 1 x 1 groups, with reflected 32 x 1 x 1 threads. The observed storage binding was 23,360 bytes; the translated shader's calculated access end was 22,016 bytes. Samples did not show binding-length overflow, but do not validate every dispatch, actual mapping, resource lifetime or synchronization.

Separate tests with float atomic lowering, compute argument preparation serialized, chunk preparation serialized, per-dispatch separate compute encoders, and GPU read fences still encountered GPU Hang. They are not sufficient fixes; read fences also change mesh-count input handling by default.

Next: investigate this task's texture and read/write storage dependencies and translation semantics while retaining its output; repeat matched baseline/candidate tests beyond the opening and verify saves and visuals.

No keys, game resources, saves, generated game shaders or raw runtime logs are included.

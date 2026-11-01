# Recorded shaders and pipelines finish before gameplay

Package Odyssey's shared pipeline recipes beside the executable and rename their shader references through the seed
alias map. Complete known warm-up before the first frame, with ODYSSEY_WARM_BEFORE_PLAY=0 retaining the title-only wait.
Draw-skipping translation becomes opt-in (TOTK_SHADER_ASYNC=1), matching the newer TOTK/BOTW default. This moves known
compilation to loading; new unrecorded states can still stall. No claim is made that an intermittent flicker is fixed.

## Source

Pipeline list export and seed alias renaming follow the root build_app.py packager, using only Odyssey's own
copied scripts and list. Draw-skipping default follows TOTK 22a4aae. No other game's runtime files are read at build time.

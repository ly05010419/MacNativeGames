# TMNT opening graphics phase

This change adds a fail-closed ownership primitive for graphics target
set/clear phases and checked rewrite rules for the supported original `main`
NSO. It does not delay window creation, sleep during startup, clear the game's
target count by timeout, or bypass its Present error branch.

The rewrite accepts only original `main` SHA-256
`cc2ce61a94ee933a31423abae8017b36bcf5b95861acf54b22d551e4a4f9d73a`
and build ID `18c437d0f8d74cc1f8ea5d927b03ec37`. Seven original ARM64
instructions are checked before nine gates are emitted. A version mismatch or
gate collision fails the rewrite. Generated game data stays outside Git.

The C owner requires the integrating runtime to supply `runtime_fail`,
`runtime_core_away`, and `runtime_core_back`, then call its gate dispatcher and
thread-exit hook. Host waits must release the guest scheduler core. The public
TMNT tree does not yet contain a playable host integration, so this commit
alone does not build a game.

Run the source tests with Python, `capstone==5.0.9`, `lz4==4.4.5`,
`cryptography`, and a C compiler:

```sh
python3 -m unittest discover -s games/tmnt/tests -p 'test_*.py'
python3 -m unittest discover -s tests
```

For a locally owned original ExeFS, use `games/tmnt/scripts/rewrite_modules.py`
to create a private output directory, then run
`games/tmnt/tests/integration_graphics_rules.py --exefs "$EXEFS" --rewritten "$REWRITTEN"`.
That check validates rewrite reports, gate placement, original instruction
preservation, and the Present branch without executing game code.

The owner probes exercise concurrency, multiple devices, capacity refusal,
post-cleanup caught/rethrow notifications, thread exit, and reuse. They are
synthetic tests. In a separate private host, two bounded silent starts of the
corresponding runtime and rewritten image reached the main menu without the
known opening crash. Neither a real managed exception during an active target
interval nor audible speaker output was verified.

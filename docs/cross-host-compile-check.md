# Cross-host compile check (no Apple SDK required)

MacNativeGames runs on Apple silicon only, and this document does not change
that. What it adds is a way to measure the **platform-neutral part** of the C
layer from any host — Linux CI, WSL, a container — without the macOS SDK,
Xcode, Vulkan headers or any game data.

```sh
tests/check_linux_compile.sh            # report only, exit 0
tests/check_linux_compile.sh --verbose  # per-file detail and first errors
```

It runs `clang -fsyntax-only` over the `.c` sources of `runtime/` and
`d2r_gfx/{gfx,nvn,tex}`, with `compat/linux` on the include path, and puts every
file in one of five buckets:

| Class | Meaning |
|---|---|
| `PASS` | Compiles and its code is active. This is the portable base. |
| `PASS*` | Compiles, but its `#if TARGET_OS_*` branch is disabled by the shim: syntax-clean, **not** functional. Never count it as portability. |
| `PORT` | Uses a real Apple API (`AudioToolbox`, `mach/`, `CoreGraphics`, `mach-o/`, `CommonCrypto`, ...). This is the actionable number. |
| `ARM` | Needs ARM intrinsics: compiles on Linux/arm64, fails on x86_64. |
| `DEP` | A library or generated header is missing (`opus.h`, `shared_fonts_config.h`). A build input, not a port task. |

`compat/linux/TargetConditionals.h` is what makes the `TARGET_OS_*` files
resolvable. It is only added on non-Apple builds, and it defines every
`TARGET_OS_*` as `0`, so no Apple branch is silently compiled.

## Baseline

Measured on 2026-10-18 with clang 23.1.1, Linux x86_64, at 1.4.3: 41 files,
11,159 LOC scanned.

| | files | LOC | % LOC |
|---|---:|---:|---:|
| `PASS` | 15 | 1,898 | 17% |
| `PASS*` | 1 | 408 | — |
| `PORT` | 23 | 8,739 | 78% |
| `DEP` | 2 | 114 | — |

The exact counts move with the compiler version, so treat the table as a
starting point and re-run before quoting it.

## Using it as a regression guard

`--min-pass N` turns the report into a ratchet: it exits non-zero when fewer
than `N` files compile.

```sh
tests/check_linux_compile.sh --min-pass 15
```

That is deliberately the only gating mode. It is **not** wired into
`.github/workflows`: the macOS job stays as it is, and nobody should be blocked
by a check for a platform this project does not ship. Keep `N` at the current
`PASS` count and raise it when a file becomes portable.

## Scope

- Syntax only. Nothing is linked, executed or rendered; a green run does not
  mean the game could run on the host.
- `.m` files are out of scope: they are Objective-C plus Metal and are reported
  by `PORT`, not compiled here.
- `PASS*` exists so a shim can never inflate the portable base.

## 中文摘要

本工具在非 Apple 主机上对 `runtime/` 与 `d2r_gfx` 的 `.c` 做语法编译检查，
用 `compat/linux/TargetConditionals.h` 把 `TARGET_OS_*` 置 0。
`PASS` 是真正可移植的部分；`PASS*` 只是语法通过、平台分支未编译，不能算作可移植；
`PORT` 才是需要移植 Apple API 的真实工作量。`--min-pass N` 可做回归门槛，
但刻意没有接入 macOS CI。仅语法检查，不代表游戏能在该主机运行。

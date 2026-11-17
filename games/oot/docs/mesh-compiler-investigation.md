# M1 Max mesh compiler crash: isolated reproduction and workaround

Investigated on 2026-11-17 against upstream `31aa41b`, Apple M1 Max
(`applegpu_g13s`), macOS 27.2.1 (26B131), full Xcode 27.1 (27A9275),
Metal 32023.921 (metalfe-32023.921.6). Command Line Tools 27.2 was also
installed; that is not a test with a full Xcode 27.2 Metal compiler.

## Symptom and isolation

The complete offline pipeline build reported two mesh and four render failures
with `AGXMetalG13X code=2`, `XPC_ERROR_CONNECTION_INTERRUPTED` after retries.
The ordinary MSL library build had completed successfully. These are failures
of the later GPU-specific pipeline compiler, not shader translation errors.

Each failed pipeline was then extracted from the same warm-up list and compiled
in its own process with `TOTK_WARM_THREADS=1`, a fresh pipeline cache, the same
runtime and the same precompiled libraries. No game world or save was loaded.

| Pipeline ID | Kind | Independent result |
|---|---|---|
| `bde4caf872443bb5a219459b5e33945ac0e64b0e` | render | success |
| `m220d2269255dde6fdde538372292c166f18c084f` | mesh | failure, repeated |
| `3a55b79c6d05c724205813ddc52f0b61e125f82b` | render | success |
| `1307baaf349794abb4c5d9597dcce6a8fbe3340c` | render | success |
| `mc29101ef3e1a498401d2f904d3a48c1976d001f2` | mesh | success, repeated |
| `3bd0b5239d1fe9d5ef8b7ac58af49bdc1c041240` | render | success |

The failing pipeline is variant 4 with these functions (all have the
`-b7530f698e76_b` binding suffix):

- task: `a7f22b4a795ff4d17e22ed243a56d0d7079f2b7afd75da4247b2db3942eee5e1`
- mesh: `ac2f3feb1142742661a038e6907a773304b34e0eaf104d3e1a238d8533e17dbd`
- fragment: `eddc12907cefc9af6c3e225c5b8167c1a9a2abb65aa88f2f41a093bc2008b876`

The other mesh uses `57ed4680…` and builds independently. Thus six parallel
failures do not establish six independently broken shaders.

## Compiler evidence

Four `MTLCompilerService` crash reports during the complete build share the
same Mesh backend stack. Isolating `m220d226…` creates the same crash reports:

```
EXC_BAD_ACCESS / SIGBUS / KERN_PROTECTION_FAILURE
llvm::MachineOperand::setReg(llvm::Register)
llvm::MachineFunctionPass::runOnFunction
llvm::AGX::AGXCompilePlan::execute
AGCModule<AGX::Impl::MeshProgramKey>::compileLLVMObject
```

The evidence supports a mesh compilation request crashing Apple's compiler
service and interrupting unrelated requests in flight. The XPC disconnection
here is local compiler IPC, not an internet connection problem. The unnamed
internal pass and the minimal MSL construct triggering it have not been reduced.

## Same-source A/B

Task and fragment libraries, runtime, pipeline row, and mesh source bytes stayed
the same. All builds retain
`-fmetal-math-mode=safe -fpreserve-invariance -fwrapv`.

An integration check exposed an important confound in the first experiment:
the successful `xcrun --sdk macosx metal -Os` invocation also selected a macOS
SDK, whereas the original build called the bare MetalToolchain driver. A
factorial follow-up, using fresh pipeline archives, separated these settings:

| Mesh compilation invocation | Pipeline result |
|---|---|
| original bare driver, default optimization | compiler crash, repeated |
| `xcrun --sdk macosx metal`, default optimization | compiler crash |
| `xcrun --sdk macosx metal -Os` | success, repeated |
| bare driver plus `-Os`, official build environment | compiler crash |
| same bare driver plus `-Os`, selected macOS `SDKROOT` | success |
| bare driver plus `-Os` and deployment-minimum flag only (26.0 or 27.0) | compiler crash |

The direct official driver emitted `air64_v29-apple-macosx28.0.0` with no
SDKROOT; the selected SDK invocation emitted a macOS 27.0 target. A target
label alone is not the explanation: forcing the deployment minimum without
the SDK environment did not solve it. **Both size optimization and the selected
macOS SDK environment are part of the tested offline workaround.** The same-SDK
default-vs-size comparison supports the optimization-path diagnosis, but the
exact internal compiler defect remains unknown.

MSL SHA-256:
`6e0e9e6f1fe97825fba77b0c4187256c6015c02845a3ff1c543bd0a1d090766d`.
The MSL is 55,947 bytes / 1,425 lines, with a 32x1x1 threadgroup.
Game-derived MSL, metallibs, pipeline archive binaries and saves are not included
in this repository or this report.

## Implementation scope

`oot_mesh_size.h` holds the exact source digest and the tested macOS major (27).
On that macOS major the OOT Mac library builder adds `-Os` for that digest only
and sets SDKROOT from `xcrun --sdk macosx --show-sdk-path` with the selected
compiler's developer environment. Its manifest records per-library options and
SDK roots and invalidates a cached library if either changes. Other libraries retain
their existing cache identities and compilation flags. Packaging checks the
option record as well as the normal content digests.

The macOS 27 OOT runtime uses `MTLLibraryOptimizationLevelSize` for the same
exact source when it must compile it itself. It copies the shared safe compile
options instead of mutating them across workers. Other macOS majors, other titles, iOS, other source
digests, and untested modified/binding variants keep their existing options.
This single-source Mac policy has been tested on M1 Max; other Mac GPUs have
not been exercised. It does not alter source semantics, reduce resolution,
enable fast math, suppress failures, or omit a draw.

## Separate issue: runtime GPU hang

Earlier castle-river gameplay failures reported
`MTL4CommandQueueErrorDomain` / `kIOGPUCommandBufferCallbackErrorHang`, followed
by the application's failure handling. These occur while executing GPU work;
the compiler crash above occurs while creating a pipeline on the CPU.

The recorded candidate pipeline IDs of the first failed submissions in three
older river runs do not include the six IDs above. Those labels record CPU
encoding, not proof of GPU execution or fault attribution. This neither proves
nor rules out a relationship. This workaround **does not claim to fix the
river GPU hang**, prove all translated MSL semantics, or establish gameplay
correctness from a compilation-only test.

## Regression checks

`tests/test_oot_mesh_size.py` uses synthetic source only: exact-digest policy,
default -> size -> unchanged -> default cache transitions, missing SDK provenance,
untested OS majors, and executable
Objective-C probes of the production option selector for OOT Mac, another
title, and the iOS branch. The probes verify safe math, invariance and the
unchanged shared options. Related library, packaging, express update and
translator tests also run before installation.

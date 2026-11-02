# x86recomp

Static recompilation of x86 and x86-64 Windows games into native 64-bit
programs: the scaffold that gets a game running natively while its code
is replaced, piece by piece, with hand-written C++. A game's machine
code is translated ahead of time into C++ and linked against a runtime
that reimplements the parts of Windows and its middleware the game
uses. There is no emulator, interpreter or Wine in
the result, and no game code or data in this repository.

The game ports built on it and their status are on the
[project site](https://jtm-rootstorm.github.io/x86recomp.site/).

## What is here

- **Lifter** (`tools/lifter`, `recomp_lift`): decodes a PE with Zydis and
  emits C++ for every function, one basic block at a time, with exact
  x86 integer and flag semantics. Straight-line x87 code becomes guarded
  "runs" that keep the FPU register stack in locals. 32-bit and x64
  images both lift; x64 code runs on the Win64 ABI against a fixed CPU
  model (AMD family 16h; SSE3 to SSE4.1, AVX and LZCNT so far).
- **x87 model** (`src/runtime/x87.cpp`): exact 80-bit x87 (precision and
  rounding control, status-word flags) on Berkeley SoftFloat with
  integer fast paths, tested bit for bit against a real x87.
- **SSE and AVX** (`src/runtime/sse.cpp`): the XMM/YMM registers and MXCSR,
  with SoftFloat lanes and x86 NaN rules, tested bit for bit against the
  CPU like the x87 model.
- **Runtime** (`src/runtime`): the guest address space (4 GiB for
  32-bit guests), PE loading, threads, SEH and x64 unwinding, crash and
  hang reports.
- **HLE** (`src/runtime/hle`): Win32 (kernel32, user32, gdi32, advapi32,
  shfolder, ...) on POSIX and SDL3; optional modules for Miles Sound
  System (with EAX 1-3 on our own reverb, modelled on Creative
  hardware), Bink, DirectDraw 7/Direct3D 7 and OpenGL 1.x (on a
  fixed-function device: SDL_GPU on Vulkan or native Metal, or
  dxvk-native's Direct3D 9 as a reference), Direct3D 9 and D3DX 9 (in
  progress), DirectSound 8, DirectInput 8, XInput and dbghelp; for x64
  guests the UCRT and MSVCP140 (in progress).
- **Shader translators** (`src/runtime/hlsl`, `recomp_hlslc`): D3DX 9's
  HLSL dialect and compiled Direct3D 9 shaders (shader models 1-3) to
  GLSL, for the programmable pipeline on SDL_GPU.
- **Tools**: Ghidra export scripts (`tools/ghidra`), a PE symbol dumper
  (`tools/pe_symbols.py`), dev-time reference checks (`tools/layout`,
  `tools/undname`), and EAX reference measurement (`tools/eax_capture`:
  a Windows 98 recorder for Creative hardware, the same cases rendered by
  the runtime, and their comparison).

Consoles whose CPU the lifter already models are to be platform modules
on the same core rather than forks: the first, for the PlayStation 4
(its executable format, system libraries and GPU), is in progress on
its own branch.

[ARCHITECTURE.md](ARCHITECTURE.md) describes the design; `docs/` the
observed behaviour of the middleware reimplemented here
([docs/miles.md](docs/miles.md)).

## Using it from a game project

```cmake
set(X86RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../x86recomp" CACHE PATH "")
set(CMAKE_LINK_DEPENDS_NO_SHARED ON)  # no relink when only a library changes
enable_testing()
add_subdirectory("${X86RECOMP_DIR}" x86recomp)

recomp_add_lifted(mygame_lifted "${CMAKE_CURRENT_SOURCE_DIR}/generated")
add_executable(mygame src/main.cpp $<TARGET_OBJECTS:mygame_lifted>)
target_link_libraries(mygame PRIVATE recomp_runtime recomp_mss32)
```

`main()` fills a `recomp::GameConfig` (id, guest install directory, EXE
name, registry seed, optional hooks) and calls `recomp::RunProcess`.
The lifter runs as a separate step over the user's own EXE:

```sh
recomp_lift --exe game.exe --symbols symbols --out generated \
  --manifest lifter.manifest
```

DLLs the game loads itself are lifted in the same run (`--module
game.dll --symbols dir [--manifest file]`, repeated) and listed in
`GameConfig::lifted_dlls`.

## Requirements

Linux x86-64, or macOS on Apple Silicon. Clang and LLVM (the static
`libLLVMDemangle.a` for dbghelp), CMake 3.25+, Ninja, SDL3 (3.2+ for
SDL_GPU); ffmpeg for Miles/Bink; glslang (`glslangValidator`) for the
shaders, and on macOS SPIRV-Cross (`spirv-cross`) too. Graphics (D3D7
and OpenGL) use SDL_GPU: Vulkan on Linux, native Metal on macOS. On
Linux a dxvk-native build (Direct3D 9 only, SDL3 window system) is
optional: it adds the d3d9 backend (`<ID>_GFX=d3d9`), the reference
image for SDL_GPU.

On macOS with Homebrew
(`brew install cmake ninja sdl3 ffmpeg llvm@22 glslang spirv-cross`):

```sh
cmake --preset debug -DRECOMP_LLVM_ROOT=/opt/homebrew/opt/llvm@22
```

## Licence

MIT (see [LICENSE](LICENSE)). Dependencies keep their own licences:
Berkeley SoftFloat 3e (BSD-3-Clause) and Zydis (MIT) are fetched at
build time; SDL3 (zlib), DXVK (zlib), ffmpeg (LGPL, linked
dynamically) and LLVM's demangler (Apache-2.0 with LLVM
exception) come from your system.

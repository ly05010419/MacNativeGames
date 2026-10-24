GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(addprefix build/gfx/,$(addsuffix .o,$(GFX_SRC)))
NATIVE_OPT ?= -O2
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn/include

PY := .venv/bin/python
# GPU work outside build/totk-native waits for the game slot too (the game binary takes it itself): one game or GPU
# probe at a time, see scripts/dev/game_slot.py. TOTK_SLOT_OWNER (claude, codex ...) names who is waiting.
GAME_SLOT_RUN = python3 scripts/dev/game_slot.py run --no-retry --purpose "make $@" --
ROM ?= ../TOTK [0100F2C0115B6000][v0].xci
KEYS ?= $(HOME)/Library/Application Support/Ryujinx/system/prod.keys

.PHONY: help dependencies extract modules probe probe-call rea-rtld rewrite-sdk rewrite-main verify test-call-window
help:
	@echo 'targets: dependencies extract modules probe probe-call rea-rtld rewrite-sdk rewrite-main verify test-call-window'
	@echo 'probe executes two original leaf functions; no game entry is executed.'

dependencies:
	@test -x "$(PY)" || python3 -m venv .venv
	"$(PY)" -m pip install -r requirements.txt

extract:
	"$(PY)" scripts/extract_exefs.py "$(ROM)" --keys "$(KEYS)" --output extracted/v0/exefs

modules:
	"$(PY)" scripts/prepare_modules.py extracted/v0/exefs --output extracted/v0/elf
	"$(PY)" scripts/map_dependencies.py extracted/v0/elf --output analysis/dependency-map.json > analysis/dependency-summary.json

build/native_leaf_probe: scripts/native_leaf_probe.c
	mkdir -p build
	xcrun clang -std=c11 -O2 -Wall -Wextra -Werror "$<" -o "$@"

probe: build/native_leaf_probe
	build/native_leaf_probe extracted/v0/elf/main.elf > analysis/native-leaf-probe.json
	@cat analysis/native-leaf-probe.json

rea-rtld:
	bash scripts/rea.sh function "$(CURDIR)/extracted/v0/elf/rtld.elf" 0x1014f0 --provider ghidra --snapshot "$(CURDIR)/analysis/rtld-clear-snapshot.json" --json > analysis/rtld-clear-function.json

rewrite-sdk:
	@mkdir -p rewritten/v0
	DM_RULESET=native DM_PRIVATE_GATES=0 "$(PY)" third_party/d2r/patch_nso.py extracted/v0/exefs/sdk rewritten/v0 sdk
	DM_RULESET=native DM_PRIVATE_GATES=0 "$(PY)" third_party/d2r/patch_nso.py extracted/v0/exefs/subsdk0 rewritten/v0 subsdk0

rewrite-main:
	@mkdir -p rewritten/v0
	DM_RULESET=native DM_PRIVATE_GATES=0 "$(PY)" third_party/d2r/patch_nso.py extracted/v0/exefs/main rewritten/v0 main

build/native_call_probe: scripts/native_call_probe.c
	mkdir -p build
	xcrun clang -std=c11 -O2 -Wall -Wextra -Werror "$<" -o "$@"

build/native-call-fixture.bin: scripts/emit_native_call_fixture.py third_party/d2r/call_window.py
	mkdir -p build
	"$(PY)" scripts/emit_native_call_fixture.py "$@"

probe-call: build/native_call_probe build/native-call-fixture.bin
	build/native_call_probe build/native-call-fixture.bin > analysis/native-call-probe.json
	@cat analysis/native-call-probe.json

test-call-window:
	"$(PY)" scripts/test_call_window.py

verify:
	"$(PY)" scripts/verify_rewrites.py

.PHONY: runtime probe-loader boot
build/runtime_tables.h: scripts/prepare_runtime.py analysis/phase12-validation.json
	"$(PY)" scripts/prepare_runtime.py

build/totk-native: Makefile runtime/loader.c runtime/runtime.h runtime/game_patches.c runtime/game_patches.h runtime/game_version.h runtime/world_survey.c runtime/world_survey.h runtime/game_slot.c runtime/services.c runtime/gate.S runtime/hle_stack.S runtime/hle_stack.c runtime/romfs.c runtime/romfs_path.h runtime/threads.c runtime/thread_compat.h runtime/thread_enter.S runtime/ipc.h runtime/audio_renderer.h runtime/audio_mixer.h runtime/audio_reverb.h runtime/audio_effects.h runtime/fs_host.h runtime/autoload.h runtime/autoload.c build/runtime_tables.h build/romfs_config.h build/shared_fonts_config.h runtime/gfx_bridge.c runtime/frontend.m runtime/account.c runtime/input.c runtime/hid_feedback.c runtime/motion_fusion.c runtime/gamepad.m runtime/gamepad_layout.h runtime/switch_left_fallback.h runtime/gamepad_feedback.m runtime/gfx_probe.c runtime/vpool_probe.m runtime/gpu_path_probe.m runtime/memory_probe.c runtime/shared_font.c runtime/ipc.c runtime/audio_renderer.c runtime/audio_mixer.c runtime/audio_reverb.c runtime/audio_effects.c runtime/fs_host.c runtime/opus_codec.c runtime/movie_host.c runtime/movie_host.h runtime/time_host.c runtime/applet_host.c runtime/swkbd_host.c $(GFX_OBJECTS)
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -Ithird_party/d2r_gfx/gfx/nvn/include -Ibuild -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include runtime/loader.c runtime/game_patches.c runtime/world_survey.c runtime/game_slot.c runtime/services.c runtime/romfs.c runtime/threads.c runtime/gate.S runtime/hle_stack.S runtime/hle_stack.c runtime/thread_enter.S runtime/gfx_bridge.c runtime/frontend.m runtime/account.c runtime/input.c runtime/hid_feedback.c runtime/motion_fusion.c runtime/autoload.c runtime/gamepad.m runtime/gamepad_feedback.m runtime/gfx_probe.c runtime/vpool_probe.m runtime/gpu_path_probe.m runtime/memory_probe.c runtime/shared_font.c runtime/ipc.c runtime/audio_renderer.c runtime/audio_mixer.c runtime/audio_reverb.c runtime/audio_effects.c runtime/fs_host.c runtime/opus_codec.c runtime/movie_host.c runtime/time_host.c runtime/applet_host.c runtime/swkbd_host.c $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@"

# Both warm-up lists come from the one shared shader list (docs/shader-list.md): pso_title.tsv is what the title screen,
# its menu and the save list use, the only list the first frame waits for (nvn_mtl.m warm_start); pso_manifest.tsv the rest.
# Each game has its own list: SHADER_LIST names it (Breath of the Wild's is games/botw/assets/shader_list.tsv; the
# botw160 targets default to it, build_app.py passes it).
SHADER_LIST ?= assets/shader_list.tsv
build/pso_manifest.tsv: $(SHADER_LIST) scripts/shader_list.py
	@mkdir -p build
	"$(PY)" scripts/shader_list.py export $(SHADER_LIST) build/pso_manifest.tsv build/pso_title.tsv
build/pso_title.tsv: build/pso_manifest.tsv

runtime: build/totk-native build/pso_manifest.tsv build/pso_title.tsv

probe-loader: runtime
	"$(PY)" scripts/verify_loader.py

boot: runtime
	build/totk-native rewritten/v0 boot

build/shared_fonts_config.h: scripts/prepare_shared_fonts.py extracted/firmware/shared-fonts/manifest.json
	"$(PY)" scripts/prepare_shared_fonts.py

build/romfs_config.h: scripts/prepare_romfs_backend.py extracted/v0/romfs/index.json
	"$(PY)" scripts/prepare_romfs_backend.py

.PHONY: probe-romfs verify-boot romfs-patches
# Text entry with the Mac keyboard (nn::swkbd::ShowKeyboard, runtime/swkbd_host.c): the guest SDK builds the arguments and the
# call goes through main's own PLT stub; answers come from TOTK_SWKBD_ANSWER (no window). Negative control 22 ignores the
# game's maximum length and must fail.
# A window close in the middle of a save waits for it (runtime/fs_host.c runtime_fs_save_busy, runtime/frontend.m).
# Negative control 23: commit does not end the save, must fail.
probe-save: runtime
	build/totk-native rewritten/v0 probe-save > analysis/native-probe-save.json
	@cat analysis/native-probe-save.json
	@if TOTK_PROBE_NEGATIVE=23 build/totk-native rewritten/v0 probe-save > /dev/null 2>&1; then echo "save negative control passed unexpectedly"; exit 1; else echo "save negative control (commit does not end the save) failed as expected"; fi

probe-swkbd: runtime
	build/totk-native rewritten/v0 probe-swkbd > analysis/native-probe-swkbd.json
	@cat analysis/native-probe-swkbd.json
	@if TOTK_PROBE_NEGATIVE=22 build/totk-native rewritten/v0 probe-swkbd > /dev/null 2>&1; then echo "swkbd negative control passed unexpectedly"; exit 1; else echo "swkbd negative control (maximum length ignored) failed as expected"; fi

probe-romfs: runtime
	"$(PY)" scripts/verify_romfs.py
# RomFS overlays served in place of the originals (runtime/romfs.c): quick-menu background capture timing at 60 fps, and the
# HeapDef sizes a true-2K output needs (TOTK_OUTPUT_RES; other sizes: make romfs-patches OUTPUT_RES=<w>x<h>).
OUTPUT_RES ?= 2560x1440
romfs-patches:
	"$(PY)" scripts/make_romfs_patches.py
	"$(PY)" scripts/make_output_res_overlay.py $(OUTPUT_RES)

verify-boot: runtime
	"$(PY)" scripts/verify_boot.py



GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h)

build/gfx/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

.PHONY: graphics-assets shader-inventory verify-shaders probe-thread probe-system-event
graphics-assets:
	"$(PY)" scripts/prepare_graphics_assets.py

shader-inventory: graphics-assets
	tooling/dotnet/dotnet run --project scripts/shader_inventory -- extracted/v0/graphics extracted/v0/shaders-v2
	"$(PY)" scripts/verify_shader_inventory.py

verify-shaders:
	"$(PY)" scripts/verify_shader_compilation.py

build/thread-probe: scripts/thread_probe.c runtime/threads.c runtime/thread_compat.h runtime/thread_enter.S
	xcrun clang -std=c11 -O1 -g -Wall -Wextra -Werror -Ithird_party/d2r_gfx/gfx/nvn/include scripts/thread_probe.c runtime/threads.c runtime/thread_enter.S -o "$@"

probe-thread: build/thread-probe
	build/thread-probe > analysis/thread-probe.json
	@cat analysis/thread-probe.json

# CPU only, against the real threads.c: SystemEvent bindings end with their write handle, a signal wakes only the waiters of
# the signalled object, SignalToAddress writes mesosphère's value (tests/system_event_bind/probe.c; GAME_BOTW=1 for BotW's build)
build/system-event-probe: tests/system_event_bind/probe.c runtime/threads.c runtime/thread_enter.S
	xcrun clang -std=c11 -O1 -g -Wall -Wextra -Werror $(if $(GAME_BOTW),-DGAME_BOTW=1) -Ithird_party/d2r_gfx/gfx/nvn/include tests/system_event_bind/probe.c runtime/threads.c runtime/thread_enter.S -o "$@"

probe-system-event: build/system-event-probe
	build/system-event-probe | tee analysis/system-event-probe.log

build/botw-atomics-probe: tests/atomic_progress/probe.c
	xcrun clang -std=c11 -O2 -g -Wall -Wextra -Werror tests/atomic_progress/probe.c -o "$@"

.PHONY: probe-account probe-gfx
probe-account:
	"$(PY)" scripts/verify_account.py

probe-gfx: runtime
	"$(PY)" scripts/verify_gfx.py

.PHONY: probe-gpu-pool-retire probe-gpu-tex-slots probe-gpu-arg-epoch probe-gpu-vertex-streams probe-gpu-widen probe-gpu-blockrt probe-gpu-midclear probe-gpu-winding probe-gpu-tile probe-gpu-depth-output probe-gpu-srgb-view probe-gpu-ds-clear probe-gpu-ub-rename probe-gpu-ubo-snapshot probe-gpu-upload-batch probe-gpu-storage-view probe-gpu-linear-rt probe-gpu-texture-retire probe-gpu-texels-region probe-gpu-layer-merge probe-gpu-cube-merge probe-gpu-compressed-alias probe-gpu-drv-c0 probe-gpu-dispatch-chain probe-gpu-copy-alias
# GPU paths isolated after a kernel panic: each probe runs alone, under the Metal API validation layer.
probe-gpu-pool-retire probe-gpu-tex-slots probe-gpu-arg-epoch probe-gpu-vertex-streams probe-gpu-widen probe-gpu-blockrt probe-gpu-midclear probe-gpu-winding probe-gpu-tile probe-gpu-depth-output probe-gpu-srgb-view probe-gpu-ds-clear probe-gpu-ub-rename probe-gpu-ubo-snapshot probe-gpu-upload-batch probe-gpu-storage-view probe-gpu-linear-rt probe-gpu-texture-retire probe-gpu-texels-region probe-gpu-layer-merge probe-gpu-cube-merge probe-gpu-compressed-alias probe-gpu-drv-c0 probe-gpu-dispatch-chain probe-gpu-copy-alias: runtime
	MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=assert TOTK_HEADLESS=1 build/totk-native rewritten/v0 $@ > analysis/native-$@.json
	cat analysis/native-$@.json
.PHONY: probe-game-slot desktop-launcher
# One game at a time: queue order, the user's game going first and stopping an agent's game, the slot passed down
# by a wrapper, refusals of a wrong descriptor, games outside the slot, wait limit; dry runs only (no game boots).
probe-game-slot: runtime
	python3 scripts/dev/test_game_slot.py build/totk-native | tee analysis/game-slot-probe.log
# The Finder launcher (tools/desktop_launcher.m) starts the game as the user: it goes ahead of agents' tests.
# One desktop alias opens the packaged app, with its standard user-data folder.
DESKTOP_APP ?= $(HOME)/Desktop/Tears of the Kingdom  Mac.app
DESKTOP_TARGET ?= ../../out/Tears of the Kingdom.app
desktop-launcher:
	"$(PY)" tools/install_desktop_entry.py --app "$(DESKTOP_TARGET)" --desktop "$(DESKTOP_APP)"
.PHONY: probe-fps60 probe-realtime probe-fp64-fallback probe-fps-overlay
# 60 FPS: emulate the patched present-interval controller (main+0x8f6bec) against the shipping one.
probe-fps60:
	"$(PY)" scripts/verify_fps60_patch.py > analysis/native-probe-fps60.json; status=$$?; tail -c 400 analysis/native-probe-fps60.json; exit $$status
probe-realtime:
	"$(PY)" scripts/verify_realtime_patch.py
probe-fp64-fallback:
	python3 scripts/verify_fp64_fallback.py
.PHONY: probe-vertex-i2f
probe-vertex-i2f:
	xcrun clang -fobjc-arc tests/vertex_i2f/probe.m -framework Metal -framework Foundation -o build/vertex-i2f-probe
	$(GAME_SLOT_RUN) build/vertex-i2f-probe
.PHONY: clock-ocr-tool probe-game-clock
clock-ocr-tool:
	xcrun clang -fobjc-arc scripts/diag/read_game_clock.m -framework Foundation -framework Vision -framework ImageIO -framework CoreGraphics -o build/read-game-clock
probe-game-clock: clock-ocr-tool
	python3 scripts/diag/check_game_clock.py
# On-screen FPS counter: glyph pixels, bounds and present-rate measurement (CPU only).
probe-fps-overlay:
	@mkdir -p build/tests
	xcrun clang -std=c11 -O1 -g -Wall -Wextra -Werror -Ithird_party/d2r_gfx/gfx/nvn tests/fps_overlay/probe.c third_party/d2r_gfx/gfx/nvn/fps_overlay.c third_party/d2r_gfx/gfx/nvn/totk_sysload.c -framework IOKit -framework CoreFoundation -framework CoreGraphics -framework CoreText -o build/tests/fps_overlay_probe
	build/tests/fps_overlay_probe
.PHONY: probe-audio
probe-audio: runtime
	build/totk-native rewritten/v0 probe-audio > analysis/native-audio-probe.json
	cat analysis/native-audio-probe.json
	@if TOTK_AUDIO_STOP_RELEASE=0 build/totk-native rewritten/v0 probe-audio > /dev/null 2> analysis/native-audio-probe-stop-negative.log; then echo "audio stop negative control passed unexpectedly"; exit 1; \
	else echo "audio negative control (a stopped voice keeps its queued buffers) failed as expected: $$(grep -o 'audio probe: [^"]*' analysis/native-audio-probe-stop-negative.log | head -1)"; fi
.PHONY: probe-vpool
probe-vpool: runtime
	build/totk-native rewritten/v0 probe-vpool > analysis/native-vpool-probe.json
	@cat analysis/native-vpool-probe.json

.PHONY: probe-memory
probe-memory: runtime
	build/totk-native rewritten/v0 probe-memory > analysis/native-memory-probe.json
	@cat analysis/native-memory-probe.json

.PHONY: probe-input
build/input-probe: scripts/input_probe.c runtime/input.c runtime/hid_feedback.c runtime/motion_fusion.c runtime/autoload.c runtime/autoload.h runtime/runtime.h
	xcrun clang -std=c11 -O1 -Wall -Wextra -Werror scripts/input_probe.c runtime/input.c runtime/hid_feedback.c runtime/motion_fusion.c runtime/autoload.c -o "$@"
probe-input: build/input-probe
	build/input-probe > analysis/native-input-probe.json
	@cat analysis/native-input-probe.json
	@if TOTK_PROBE_NEGATIVE=20 build/input-probe > /dev/null 2> analysis/native-input-probe-negative.log; then echo "negative control 20 passed: the orientation check cannot see a reference-frame integration"; exit 1; else echo "negative control 20 (reference-frame integration) failed as expected: $$(cat analysis/native-input-probe-negative.log)"; fi

# TOTK_AUTOLOAD (runtime/autoload.c) through the production controller state; the last run is the negative control
# (auto-load off while presses are expected) and must fail.
.PHONY: probe-autoload
.PHONY: probe-movie
# Host VP9 movie::Decoder (runtime/movie_host.c) against ffmpeg's own VP9 decode of the blood-moon cutscene, frame by frame.
build/movie-probe: scripts/movie_probe.c runtime/movie_host.c runtime/movie_host.h runtime/runtime.h
	xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -I/opt/homebrew/opt/libvpx/include scripts/movie_probe.c runtime/movie_host.c /opt/homebrew/opt/libvpx/lib/libvpx.a -o "$@"
probe-movie: build/movie-probe
	python3 scripts/movie_probe_inputs.py analysis/movie-probe
	build/movie-probe analysis/movie-probe/frames.ivf analysis/movie-probe/reference.nv12 $$(cat analysis/movie-probe/meta.txt) buffers | tee analysis/movie-probe.json
	build/movie-probe analysis/movie-probe/frames.ivf analysis/movie-probe/reference.nv12 $$(cat analysis/movie-probe/meta.txt) paced | tee -a analysis/movie-probe.json
build/autoload-probe: scripts/autoload_probe.c runtime/autoload.c runtime/autoload.h runtime/input.c runtime/hid_feedback.c runtime/motion_fusion.c runtime/runtime.h
	xcrun clang -std=c11 -O1 -Wall -Wextra -Werror scripts/autoload_probe.c runtime/autoload.c runtime/input.c runtime/hid_feedback.c runtime/motion_fusion.c -o "$@"
probe-autoload: build/autoload-probe
	TOTK_AUTOLOAD=1 TOTK_AUTOLOAD_PERIOD_MS=400 build/autoload-probe press
	TOTK_AUTOLOAD=1 TOTK_AUTOLOAD_PERIOD_MS=400 build/autoload-probe nosave
	TOTK_AUTOLOAD=1 TOTK_AUTOLOAD_PERIOD_MS=400 build/autoload-probe key
	TOTK_AUTOLOAD=1 TOTK_AUTOLOAD_PERIOD_MS=400 build/autoload-probe pad
	TOTK_AUTOLOAD=1 TOTK_AUTOLOAD_PERIOD_MS=400 TOTK_AUTOLOAD_LIMIT_S=1 build/autoload-probe limit
	env -u TOTK_AUTOLOAD build/autoload-probe off
	TOTK_AUTOLOAD=1 TOTK_AUTOLOAD_PERIOD_MS=400 TOTK_INPUT_SCRIPT= build/autoload-probe off
	@if env -u TOTK_AUTOLOAD TOTK_AUTOLOAD_PERIOD_MS=400 build/autoload-probe press 2>/dev/null; then echo "autoload negative control passed unexpectedly"; exit 1; \
	 else echo "autoload negative control (auto-load off, presses expected) failed as expected"; fi

.PHONY: shader-tools
# The pinned Ryujinx shader translator is copied out of the D2R checkout and patched (tools/totk_xlate/patches).
RYU_PINNED := tooling/d2r-mobile/dm/third_party/ryujinx
shader-tools:
	rm -rf build/ryujinx-src && mkdir -p build/ryujinx-src && cp -R $(RYU_PINNED)/. build/ryujinx-src/
	find build/ryujinx-src/src -maxdepth 2 \( -name obj -o -name bin \) -type d -exec rm -rf {} +
	for p in tools/totk_xlate/patches/*.patch; do patch -d build/ryujinx-src -p1 --forward < "$$p" || exit 1; done
	tooling/dotnet/dotnet build tools/totk_xlate/totk_xlate.csproj -c Release -o build/totk_xlate -p:RyuSrc=../../build/ryujinx-src

.PHONY: shader-service
# The runtime shader translation service as one native program (2026-10-17): the patched translator of make shader-tools
# plus tools/totk_xlate/Service.cs (the port of scripts/shader_service.py), published with NativeAOT. The runtime uses
# build/shader-service/totk_xlate when it is there (no Python, no .NET runtime; TOTK_SHADER_SERVICE=python for the old
# path). Check a change: scripts/diag/retranslate_cache.py --service build/shader-service/totk_xlate --compare <cache>.
shader-service:
	@test -d build/ryujinx-src || { echo "make shader-tools first (it prepares the patched translator sources)"; exit 1; }
	tooling/dotnet/dotnet publish tools/totk_xlate/totk_xlate.csproj -c Release -r osx-arm64 -p:PublishAot=true \
	  -p:InvariantGlobalization=true -p:RyuSrc=../../build/ryujinx-src -o build/shader-service

.PHONY: graphics-tools
graphics-tools:
	tooling/dotnet/dotnet build tooling/d2r-mobile/dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj -c Release -o build/dmsh_xlate
	cmake -S tooling/spirv-cross -B build/spirv-cross -DCMAKE_BUILD_TYPE=Release -DSPIRV_CROSS_ENABLE_TESTS=OFF
	cmake --build build/spirv-cross -j 4

.PHONY: probe-metal-semantics
# GPU: Metal behaviours the shader translation relies on (2026-10-15): min / max / clamp / saturate with a NaN operand
# return the other one, safe math keeps NaN * 0 = NaN, a colour attachment the fragment function does not write is kept.
probe-metal-semantics:
	@mkdir -p build analysis
	xcrun clang -fobjc-arc -O1 -Wall -Wextra -Werror tests/metal_semantics/probe.m -framework Metal -framework Foundation -o build/metal-semantics-probe
	$(GAME_SLOT_RUN) build/metal-semantics-probe | tee analysis/metal-semantics-probe.log

.PHONY: probe-bc1astc
# BC1 -> ASTC 4x4 transcoding for GPUs without BC (iPhone A15/A16), checked through Metal's ASTC decoder; CPU and GPU agree.
probe-bc1astc:
	@mkdir -p build
	xcrun clang -fobjc-arc -O1 -Wall -Wextra -Werror -Wno-unused-parameter tests/bc1astc/probe.m third_party/d2r_gfx/gfx/tex/nvn_bc1astc.c -framework Metal -framework Foundation -o build/bc1astc-probe
	build/bc1astc-probe | tee analysis/probe-bc1astc.json

.PHONY: probe-gpu-bctc
# GPU transcoder for GPUs without BC (tex/nvn_bctc.metal): BC2/BC3/BC7 -> ASTC 4x4, BC4/BC5 -> EAC, decoded by Metal and
# compared with bcdec (kernel decoders exact, smooth-block PSNR, dual-plane ASTC on representable blocks).
probe-gpu-bctc:
	@mkdir -p build analysis
	xcrun clang -fobjc-arc -O1 -Wall -Wextra -Werror -Wno-unused-parameter tests/bctc/probe.m -framework Metal -framework Foundation -o build/bctc-probe
	build/bctc-probe | tee analysis/probe-gpu-bctc.json

.PHONY: probe-metalfx-interp
# GPU: MetalFX frame interpolation with synthetic frames of known motion (PSNR against the analytic middle frame): motion
# units and sign, depth / motion smaller than the output, premultiplied UI given apart, Metal 4 encoder, GPU time per
# interpolated frame at several sizes; negative controls with flipped / zero / wrongly scaled motion. Run alone (GPU).
probe-metalfx-interp:
	@mkdir -p build analysis
	xcrun clang -fobjc-arc -O1 -Wall -Wextra -Werror -Wno-unused-parameter tests/metalfx_interp/probe.m -framework Metal -framework MetalFX -framework Foundation -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers -o build/metalfx-interp-probe
	$(GAME_SLOT_RUN) build/metalfx-interp-probe | tee analysis/metalfx-interp-probe.log

.PHONY: probe-dirty-race
# CPU only: dirty-page tracker, "set read-only" racing "unprotect before a host write" (from D2R Mobile ef9b1ff), with a negative control.
probe-dirty-race:
	@mkdir -p build analysis
	xcrun clang -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-unused-function -Ithird_party/d2r_gfx/gfx/nvn tests/dirty_race/probe.c -o build/dirty-race-probe
	build/dirty-race-probe | tee analysis/dirty-race-probe.log

.PHONY: probe-pool-index
# CPU only: memory pool address index vs the old linear scan, with two negative controls and a timing line.
probe-pool-index:
	@mkdir -p build analysis
	xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -Ithird_party/d2r_gfx/gfx/nvn tests/pool_index/probe.c -o build/pool-index-probe
	build/pool-index-probe | tee analysis/pool-index-probe.log

.PHONY: probe-reverb
# Reference binaries are local tools, not part of the shipped runtime.
probe-reverb:
	@test -n "$(RYUJINX_AUDIO_REF)" || (echo 'Set RYUJINX_AUDIO_REF to the directory containing Ryujinx.Audio.dll'; exit 2)
	tooling/dotnet/dotnet run --project tests/reverb/Reference.csproj -p:ReferenceRoot="$(RYUJINX_AUDIO_REF)" -- analysis/reverb-reference.bin analysis/limiter-reference.bin
	xcrun clang -std=c11 -O1 -Wall -Wextra -Werror -Iruntime tests/reverb/probe.c runtime/audio_reverb.c -o build/reverb-probe
	build/reverb-probe analysis/reverb-reference.bin

.PHONY: probe-audio-effects
probe-audio-effects: runtime probe-reverb
	TOTK_LIMITER_REFERENCE=analysis/limiter-reference.bin build/totk-native rewritten/v0 probe-audio

# Host-only validation of live survey parsing and actor pointer traversal; no GPU or game.
.PHONY: probe-world-survey
build/world-survey-probe: scripts/world_survey_probe.c runtime/world_survey.c runtime/world_survey.h runtime/runtime.h
	xcrun clang -std=c11 -O1 -Wall -Wextra -Werror scripts/world_survey_probe.c -o "$@"
probe-world-survey: build/world-survey-probe
	build/world-survey-probe
	@TOTK_PROBE_NEGATIVE=1 build/world-survey-probe > analysis/world-survey-negative.log 2>&1; test $$? -eq 1
	@rg 'FAIL: position actually changed' analysis/world-survey-negative.log

# Isolated 1.1.2 build; never replace the validated v0 executable or generated headers.
.PHONY: runtime-v112
build/v112/runtime_tables.h: scripts/prepare_runtime.py analysis/v112-port/phase12-validation.json
	"$(PY)" scripts/prepare_runtime.py --version v112
build/v112/romfs_config.h: scripts/prepare_romfs_backend.py extracted/v112/romfs/index.json
	"$(PY)" scripts/prepare_romfs_backend.py --version v112
build/totk-native-v112: $(wildcard runtime/*.[ch] runtime/*.m runtime/*.S) build/v112/runtime_tables.h build/v112/romfs_config.h build/shared_fonts_config.h $(GFX_OBJECTS) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -Ithird_party/d2r_gfx/gfx/nvn/include -DTOTK_GAME_V112=1 -Ibuild/v112 -Ibuild -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include runtime/loader.c runtime/game_patches.c runtime/world_survey.c runtime/game_slot.c runtime/services.c runtime/romfs.c runtime/threads.c runtime/gate.S runtime/hle_stack.S runtime/hle_stack.c runtime/thread_enter.S runtime/gfx_bridge.c runtime/frontend.m runtime/account.c runtime/input.c runtime/hid_feedback.c runtime/motion_fusion.c runtime/autoload.c runtime/gamepad.m runtime/gamepad_feedback.m runtime/gfx_probe.c runtime/vpool_probe.m runtime/gpu_path_probe.m runtime/memory_probe.c runtime/shared_font.c runtime/ipc.c runtime/audio_renderer.c runtime/audio_mixer.c runtime/audio_reverb.c runtime/audio_effects.c runtime/fs_host.c runtime/opus_codec.c runtime/movie_host.c runtime/time_host.c runtime/applet_host.c runtime/swkbd_host.c $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@"
runtime-v112: build/totk-native-v112 build/pso_manifest.tsv build/pso_title.tsv

# Isolated 1.2.1 build (the same SDK modules as 1.0.0 and 1.1.2); never replace the validated v0 executable or generated headers.
.PHONY: runtime-v121
build/v121/runtime_tables.h: scripts/prepare_runtime.py analysis/v121-port/phase12-validation.json
	"$(PY)" scripts/prepare_runtime.py --version v121
build/v121/romfs_config.h: scripts/prepare_romfs_backend.py extracted/v121/romfs/index.json
	"$(PY)" scripts/prepare_romfs_backend.py --version v121
build/totk-native-v121: $(wildcard runtime/*.[ch] runtime/*.m runtime/*.S) build/v121/runtime_tables.h build/v121/romfs_config.h build/shared_fonts_config.h $(GFX_OBJECTS) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -Ithird_party/d2r_gfx/gfx/nvn/include -DTOTK_GAME_V121=1 -Ibuild/v121 -Ibuild -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include runtime/loader.c runtime/game_patches.c runtime/world_survey.c runtime/game_slot.c runtime/services.c runtime/romfs.c runtime/threads.c runtime/gate.S runtime/hle_stack.S runtime/hle_stack.c runtime/thread_enter.S runtime/gfx_bridge.c runtime/frontend.m runtime/account.c runtime/input.c runtime/hid_feedback.c runtime/motion_fusion.c runtime/autoload.c runtime/gamepad.m runtime/gamepad_feedback.m runtime/gfx_probe.c runtime/vpool_probe.m runtime/gpu_path_probe.m runtime/memory_probe.c runtime/shared_font.c runtime/ipc.c runtime/audio_renderer.c runtime/audio_mixer.c runtime/audio_reverb.c runtime/audio_effects.c runtime/fs_host.c runtime/opus_codec.c runtime/movie_host.c runtime/time_host.c runtime/applet_host.c runtime/swkbd_host.c $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@"
runtime-v121: build/totk-native-v121 build/pso_manifest.tsv build/pso_title.tsv

# Isolated 1.4.3 build; never replace the validated v0 executable or generated headers.
.PHONY: runtime-v143
build/v143/runtime_tables.h: scripts/prepare_runtime.py analysis/v143-port/phase12-validation.json
	"$(PY)" scripts/prepare_runtime.py --version v143
build/v143/romfs_config.h: scripts/prepare_romfs_backend.py extracted/v143/romfs/index.json
	"$(PY)" scripts/prepare_romfs_backend.py --version v143
build/totk-native-v143: $(wildcard runtime/*.[ch] runtime/*.m runtime/*.S) build/v143/runtime_tables.h build/v143/romfs_config.h build/shared_fonts_config.h $(GFX_OBJECTS) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -Ithird_party/d2r_gfx/gfx/nvn/include -DTOTK_GAME_V143=1 -Ibuild/v143 -Ibuild -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include runtime/loader.c runtime/game_patches_v143.c runtime/world_survey.c runtime/game_slot.c runtime/services.c runtime/romfs.c runtime/threads.c runtime/gate.S runtime/hle_stack.S runtime/hle_stack.c runtime/thread_enter.S runtime/gfx_bridge.c runtime/frontend.m runtime/account.c runtime/input.c runtime/hid_feedback.c runtime/motion_fusion.c runtime/autoload.c runtime/gamepad.m runtime/gamepad_feedback.m runtime/gfx_probe.c runtime/vpool_probe.m runtime/gpu_path_probe.m runtime/memory_probe.c runtime/shared_font.c runtime/ipc.c runtime/audio_renderer.c runtime/audio_mixer.c runtime/audio_reverb.c runtime/audio_effects.c runtime/fs_host.c runtime/opus_codec.c runtime/movie_host.c runtime/time_host.c runtime/applet_host.c runtime/swkbd_host.c $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@"
runtime-v143: build/totk-native-v143 build/pso_manifest.tsv build/pso_title.tsv

# Keep standalone rendering probe binaries outside the source checkout.
RENDER_PROBE_DIR ?= $(abspath $(CURDIR)/../../../TOTK_build/rendering-probes)
.PHONY: probe-texture-auto-layout
$(RENDER_PROBE_DIR)/texture-auto-layout-probe: tests/texture_auto_layout/probe.c third_party/d2r_gfx/gfx/nvn/nvn_tex.c third_party/d2r_gfx/gfx/nvn/nvn_tex.h third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c
	@mkdir -p "$(RENDER_PROBE_DIR)"
	xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -Ithird_party/d2r_gfx/gfx/nvn -Ithird_party/d2r_gfx/gfx/tex tests/texture_auto_layout/probe.c third_party/d2r_gfx/gfx/nvn/nvn_tex.c third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c -o "$@"
probe-texture-auto-layout: $(RENDER_PROBE_DIR)/texture-auto-layout-probe
	"$(RENDER_PROBE_DIR)/texture-auto-layout-probe"

.PHONY: probe-depth16-clear
$(RENDER_PROBE_DIR)/depth16-clear-probe: tests/depth16_clear/probe.m third_party/d2r_gfx/gfx/nvn/nvn_depth_clear.h
	@mkdir -p "$(RENDER_PROBE_DIR)"
	xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn tests/depth16_clear/probe.m -framework Metal -framework Foundation -o "$@"
probe-depth16-clear: $(RENDER_PROBE_DIR)/depth16-clear-probe
	$(GAME_SLOT_RUN) "$(RENDER_PROBE_DIR)/depth16-clear-probe"

.PHONY: probe-fbread
.PHONY: probe-gpu-writeback
probe-gpu-writeback: runtime-botw160
	MTL_DEBUG_LAYER=1 build/totk-native-botw160 rewritten/botw160 probe-gpu-writeback

# Decals reading the attachment they write (nvn_fbread.h): overlapping draws see each other only through the tile read.
$(RENDER_PROBE_DIR)/fbread-probe: tests/fbread/probe.m third_party/d2r_gfx/gfx/nvn/nvn_fbread.h
	@mkdir -p "$(RENDER_PROBE_DIR)"
	xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn tests/fbread/probe.m -framework Metal -framework Foundation -o "$@"
probe-fbread: $(RENDER_PROBE_DIR)/fbread-probe
	$(GAME_SLOT_RUN) "$(RENDER_PROBE_DIR)/fbread-probe"

# Independent, game-data-free DSP and production-mixer comparisons (from the original port, 2026-10-18). Keep vectors,
# .NET build output and executables outside the source checkout. Needs a .NET SDK and a Ryujinx.Audio build.
AUDIO_TEST_WORK ?= /tmp/totk-native-audio-effects
DOTNET ?= dotnet
.PHONY: test-audio-effects
test-audio-effects:
	@test -n "$(RYUJINX_AUDIO_REF)" || (echo 'Set RYUJINX_AUDIO_REF to the directory containing Ryujinx.Audio.dll'; exit 2)
	mkdir -p "$(AUDIO_TEST_WORK)"
	"$(DOTNET)" run --project tests/audio_effects/Reference.csproj -p:ReferenceRoot="$(RYUJINX_AUDIO_REF)" -p:BaseIntermediateOutputPath="$(AUDIO_TEST_WORK)/obj/" -p:OutputPath="$(AUDIO_TEST_WORK)/bin/" -- "$(AUDIO_TEST_WORK)/reference.bin"
	xcrun clang -std=c11 -O1 -g -Wall -Wextra -Werror -ffp-contract=off -fsanitize=undefined -fno-sanitize-recover=all -Iruntime tests/audio_effects/probe.c runtime/audio_effects.c -o "$(AUDIO_TEST_WORK)/dsp-probe"
	"$(AUDIO_TEST_WORK)/dsp-probe" "$(AUDIO_TEST_WORK)/reference.bin"
	xcrun clang -std=c11 -O2 -g -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all -Iruntime tests/audio_effects/mixer_probe.c runtime/audio_effects.c runtime/audio_reverb.c -framework AudioToolbox -o "$(AUDIO_TEST_WORK)/mixer-probe"
	"$(AUDIO_TEST_WORK)/mixer-probe" "$(AUDIO_TEST_WORK)/reference.bin"

# ---- The Legend of Zelda: Breath of the Wild 1.6.0 (botw.md) ----
# The shared runtime and graphics layer compiled with GAME_BOTW: the graphics objects go to build/gfx-botw, so every TOTK
# binary keeps exactly the code it had. game_patches_botw.c replaces game_patches.c.
GFX_OBJECTS_BOTW := $(addprefix build/gfx-botw/,$(addsuffix .o,$(GFX_SRC)))
build/gfx-botw/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -DGAME_BOTW=1 -c "$<" -o "$@"
.PHONY: runtime-botw160
build/botw160/runtime_tables.h: scripts/prepare_runtime.py analysis/botw160-port/phase12-validation.json
	"$(PY)" scripts/prepare_runtime.py --version botw160
build/botw160/romfs_config.h: scripts/prepare_romfs_backend.py extracted/botw160/romfs/index.json
	"$(PY)" scripts/prepare_romfs_backend.py --version botw160
# The add-on content volumes (games/botw/scripts/extract_aoc_romfs.py -> extracted/botw160/aoc/<index>); a missing index is not listed.
build/botw160/aoc_config.h: scripts/prepare_romfs_backend.py $(wildcard extracted/botw160/aoc/*/index.json)
	"$(PY)" scripts/prepare_romfs_backend.py --version botw160 --aoc
build/totk-native-botw160: $(wildcard runtime/*.[ch] runtime/*.m runtime/*.S) build/botw160/runtime_tables.h build/botw160/romfs_config.h build/botw160/aoc_config.h build/shared_fonts_config.h $(GFX_OBJECTS_BOTW) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -Ithird_party/d2r_gfx/gfx/nvn/include -DGAME_BOTW=1 -DGAME_BOTW160=1 -Ibuild/botw160 -Ibuild -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include runtime/loader.c runtime/game_patches_botw.c runtime/aoc_romfs.c runtime/settings_service.c runtime/world_survey.c runtime/game_slot.c runtime/services.c runtime/romfs.c runtime/threads.c runtime/gate.S runtime/hle_stack.S runtime/hle_stack.c runtime/thread_enter.S runtime/gfx_bridge.c runtime/frontend.m runtime/account.c runtime/input.c runtime/hid_feedback.c runtime/motion_fusion.c runtime/autoload.c runtime/gamepad.m runtime/gamepad_feedback.m runtime/gfx_probe.c runtime/vpool_probe.m runtime/gpu_path_probe.m runtime/memory_probe.c runtime/shared_font.c runtime/ipc.c runtime/audio_renderer.c runtime/audio_mixer.c runtime/audio_reverb.c runtime/audio_effects.c runtime/fs_host.c runtime/opus_codec.c runtime/movie_host.c runtime/time_host.c runtime/applet_host.c runtime/swkbd_host.c $(GFX_OBJECTS_BOTW) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@"
runtime-botw160: SHADER_LIST ?= ../botw/assets/shader_list.tsv
runtime-botw160: build/totk-native-botw160 build/pso_manifest.tsv build/pso_title.tsv
# Breath of the Wild 1.9.0 (docs/botw-v1.9.0-port.md): the same runtime, and the graphics objects compiled with GAME_BOTW190
# as well (build/gfx-botw190; the NVN version it reports differs from 1.6.0's).
GFX_OBJECTS_BOTW190 := $(addprefix build/gfx-botw190/,$(addsuffix .o,$(GFX_SRC)))
build/gfx-botw190/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -DGAME_BOTW=1 -DGAME_BOTW190=1 -c "$<" -o "$@"
.PHONY: runtime-botw190
build/botw190/runtime_tables.h: scripts/prepare_runtime.py analysis/botw190-port/phase12-validation.json
	"$(PY)" scripts/prepare_runtime.py --version botw190
build/botw190/romfs_config.h: scripts/prepare_romfs_backend.py extracted/botw190/romfs/index.json
	"$(PY)" scripts/prepare_romfs_backend.py --version botw190
# 1.9.0: the add-on content volumes the build imported from the --dlc NSPs (extracted/botw190/aoc/<index>).
build/botw190/aoc_config.h: scripts/prepare_romfs_backend.py $(wildcard extracted/botw190/aoc/*/index.json)
	"$(PY)" scripts/prepare_romfs_backend.py --version botw190 --aoc
build/totk-native-botw190: $(wildcard runtime/*.[ch] runtime/*.m runtime/*.S) build/botw190/runtime_tables.h build/botw190/romfs_config.h build/botw190/aoc_config.h build/shared_fonts_config.h $(GFX_OBJECTS_BOTW190) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -Ithird_party/d2r_gfx/gfx/nvn/include -DGAME_BOTW=1 -DGAME_BOTW190=1 -Ibuild/botw190 -Ibuild -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include runtime/loader.c runtime/game_patches_botw.c runtime/aoc_romfs.c runtime/settings_service.c runtime/world_survey.c runtime/game_slot.c runtime/services.c runtime/romfs.c runtime/threads.c runtime/gate.S runtime/hle_stack.S runtime/hle_stack.c runtime/thread_enter.S runtime/gfx_bridge.c runtime/frontend.m runtime/account.c runtime/input.c runtime/hid_feedback.c runtime/motion_fusion.c runtime/autoload.c runtime/gamepad.m runtime/gamepad_feedback.m runtime/gfx_probe.c runtime/vpool_probe.m runtime/gpu_path_probe.m runtime/memory_probe.c runtime/shared_font.c runtime/ipc.c runtime/audio_renderer.c runtime/audio_mixer.c runtime/audio_reverb.c runtime/audio_effects.c runtime/fs_host.c runtime/opus_codec.c runtime/movie_host.c runtime/time_host.c runtime/applet_host.c runtime/swkbd_host.c $(GFX_OBJECTS_BOTW190) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@"
runtime-botw190: SHADER_LIST ?= ../botw/assets/shader_list.tsv
runtime-botw190: build/totk-native-botw190 build/pso_manifest.tsv build/pso_title.tsv
# Production Opus-wrapper regression checks; no game assets or audio device needed.
OPUS_TEST_WORK ?= /tmp/totk-native-opus-codec
OPUS_PREFIX ?= /opt/homebrew/opt/opus
OPUS_SANITIZERS ?= address,undefined
.PHONY: test-opus-codec
test-opus-codec:
	mkdir -p "$(OPUS_TEST_WORK)"
	xcrun clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=$(OPUS_SANITIZERS) -fno-sanitize-recover=all -I"$(OPUS_PREFIX)/include/opus" tests/opus_codec/probe.c "$(OPUS_PREFIX)/lib/libopus.a" -o "$(OPUS_TEST_WORK)/probe"
	"$(OPUS_TEST_WORK)/probe"
	@probe_status=0; "$(OPUS_TEST_WORK)/probe" --bad-range > "$(OPUS_TEST_WORK)/negative-range.log" 2>&1 || probe_status=$$?; test "$$probe_status" -eq 1
	@grep -q 'Opus stream integrity' "$(OPUS_TEST_WORK)/negative-range.log"

.PHONY: probe-gpu-sampler-border
probe-gpu-sampler-border: runtime-v143
	@tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; TOTK_BORDER_PLAIN=0 TOTK_PROFILE_DIR="$$tmp/profile" TOTK_SHADER_CACHE="$$tmp/cache" DM_PSO_RECORD="$$tmp/pso.tsv" $(GAME_SLOT_RUN) build/totk-native-v143 rewritten/v143 probe-gpu-sampler-border

.PHONY: probe-gpu-sampler-border-edge
probe-gpu-sampler-border-edge: probe-gpu-sampler-border

# opus, libvpx and dav1d (the AV1 fallback): build_app.py passes these (codec_libraries); a make by hand asks the
# repository's tools/deps, which makes them once per Mac from pinned sources (no Homebrew).
OPUS_PREFIX ?= $(or $(shell python3 ../../tools/deps prefix opus),$(error tools/deps could not make opus))
VPX_PREFIX ?= $(or $(shell python3 ../../tools/deps prefix libvpx),$(error tools/deps could not make libvpx))
DAV1D_PREFIX ?= $(or $(shell python3 ../../tools/deps prefix dav1d),$(error tools/deps could not make dav1d))
# Isolated copy of our native Switch 2 runtime. Build artifacts stay in the caller's external workspace.
WORK ?=
ifeq ($(strip $(WORK)),)
$(error WORK must name an external workspace)
endif
# make splits target names at spaces (#2045); build.py passes a link without them (make_path)
ifneq ($(words $(WORK)),1)
$(error WORK must not contain spaces: $(WORK) (build.py passes a link without them))
endif
OUT := $(WORK)/build
NATIVE_OPT ?= -O2
GAME_INFO := -Wl,-sectcreate,__TEXT,__info_plist,runtime/game_info.plist
GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m zeldaVR/gfx/*.c zeldaVR/gfx/*.m)
GFX_OBJECTS := $(addprefix $(OUT)/obj/,$(addsuffix .o,$(GFX_SRC)))
GFX_HEADERS := $(wildcard runtime/input_prompts.h runtime/prompt_bindings.h runtime/keyboard_map.h runtime/controller_map.h third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/*.inc third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h third_party/smaa/*.h zeldaVR/gfx/*.h zeldaVR/runtime/*.h)
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn/include -Iruntime -DGAME_OOT=1 -DGAME_NS2=1
.DEFAULT_GOAL := runtime
.PHONY: runtime
$(OUT)/obj/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"
$(OUT)/oot-native: $(wildcard runtime/*.[ch] runtime/*.m runtime/*.S) $(GFX_OBJECTS) Makefile runtime/game_info.plist app/launcher.m
	xcrun clang -std=c11 $(NATIVE_OPT) $(GAME_INFO) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -Ithird_party/d2r_gfx/gfx/nvn/include -DGAME_OOT=1 -DGAME_NS2=1 -I$(OUT) -I$(DAV1D_PREFIX)/include -I$(OPUS_PREFIX)/include/opus -I$(VPX_PREFIX)/include runtime/loader.c runtime/game_data.c runtime/game_patches_ns2.c runtime/ns2_host.c runtime/ns2_codec.c runtime/hle_movie_av1.c runtime/ns2_audio_out.c runtime/ns2_opus_ms.c runtime/offline_host.c runtime/world_survey.c runtime/game_slot.c runtime/services.c runtime/romfs.c runtime/threads.c runtime/gate.S runtime/hle_stack.S runtime/hle_stack.c runtime/thread_enter.S runtime/gfx_bridge.c runtime/frontend.m runtime/account.c runtime/input.c runtime/hid_feedback.c runtime/motion_fusion.c runtime/autoload.c runtime/gamepad.m runtime/gamepad_feedback.m runtime/gfx_probe.c runtime/vpool_probe.m runtime/gpu_path_probe.m runtime/memory_probe.c runtime/shared_font.c runtime/ipc.c runtime/audio_renderer.c runtime/audio_mixer.c runtime/audio_reverb.c runtime/audio_effects.c runtime/fs_host.c runtime/opus_codec.c runtime/movie_host.c runtime/time_host.c runtime/applet_host.c runtime/swkbd_host.c app/launcher.m runtime/cloud_save.m $(GFX_OBJECTS) $(DAV1D_PREFIX)/lib/libdav1d.a $(OPUS_PREFIX)/lib/libopus.a $(VPX_PREFIX)/lib/libvpx.a -framework VideoToolbox -framework CoreMedia -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -framework IOSurface -o "$(OUT)/oot-native"
runtime: $(OUT)/oot-native

# The leaderboard's frame-rate record (fps_board.c): average, median, 1% low, highest, interpolated frames.
$(OUT)/fps-board: tests/fps_board/probe.c third_party/d2r_gfx/gfx/nvn/fps_board.c third_party/d2r_gfx/gfx/nvn/fps_board.h
	@mkdir -p "$(OUT)"
	xcrun clang -std=c11 -O1 -g -Wall -Wextra -Werror -Ithird_party/d2r_gfx/gfx/nvn tests/fps_board/probe.c third_party/d2r_gfx/gfx/nvn/fps_board.c -o "$@"
.PHONY: probe-fps-board
probe-fps-board: $(OUT)/fps-board
	"$(OUT)/fps-board"

$(OUT)/address-wait64: tests/address_wait64.c runtime/threads.c runtime/thread_enter.S
	@mkdir -p "$(OUT)"
	xcrun clang -std=c11 -O1 -g -Wall -Wextra -Werror -DGAME_NS2=1 -DGAME_OOT=1 -Ithird_party/d2r_gfx/gfx/nvn/include $^ -o "$@"
.PHONY: probe-address-wait64
probe-address-wait64: $(OUT)/address-wait64
	"$(OUT)/address-wait64"

$(OUT)/gmem-index: tests/gmem_index.c third_party/d2r_gfx/gfx/nvn/nvn_gmem_index.h
	@mkdir -p "$(OUT)"
	xcrun clang -std=c11 -O2 -Wall -Wextra -Werror $< -o "$@"
.PHONY: probe-gmem-index
probe-gmem-index: $(OUT)/gmem-index
	"$(OUT)/gmem-index"

$(OUT)/timeline-owner: tests/timeline_owner.c third_party/d2r_gfx/gfx/nvn/totk_timeline.c third_party/d2r_gfx/gfx/nvn/totk_timeline.h
	@mkdir -p "$(OUT)"
	xcrun clang -std=gnu11 -O2 -Wall -Wextra -Werror tests/timeline_owner.c third_party/d2r_gfx/gfx/nvn/totk_timeline.c -o "$@"
.PHONY: probe-timeline-owner
probe-timeline-owner: $(OUT)/timeline-owner
	"$(OUT)/timeline-owner" "$(OUT)/timeline-owner.tsv"

$(OUT)/timeline-trigger: tests/timeline_trigger.c third_party/d2r_gfx/gfx/nvn/totk_timeline.c third_party/d2r_gfx/gfx/nvn/totk_timeline.h
	@mkdir -p "$(OUT)"
	xcrun clang -std=gnu11 -O2 -Wall -Wextra -Werror tests/timeline_trigger.c third_party/d2r_gfx/gfx/nvn/totk_timeline.c -o "$@"
.PHONY: probe-timeline-trigger
probe-timeline-trigger: $(OUT)/timeline-trigger
	"$(OUT)/timeline-trigger" "$(OUT)/timeline-trigger.tsv"

$(OUT)/pool-sources: tests/pool_sources.m third_party/d2r_gfx/gfx/nvn/nvn_pool_sources.m third_party/d2r_gfx/gfx/nvn/nvn_pool_sources.h third_party/d2r_gfx/gfx/nvn/nvn_refl.h
	@mkdir -p "$(OUT)"
	xcrun clang -std=gnu11 -O2 -Wall -Wextra -Werror -fobjc-arc tests/pool_sources.m third_party/d2r_gfx/gfx/nvn/nvn_pool_sources.m -framework Foundation -o "$@"
.PHONY: probe-pool-sources
probe-pool-sources: $(OUT)/pool-sources
	"$(OUT)/pool-sources"

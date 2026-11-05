# Isolated copy of our native Switch 2 runtime. Build artifacts stay in the caller's external workspace.
WORK ?=
ifeq ($(strip $(WORK)),)
$(error WORK must name an external workspace)
endif
OUT := $(WORK)/build
NATIVE_OPT ?= -O2
GAME_INFO := -Wl,-sectcreate,__TEXT,__info_plist,runtime/game_info.plist
GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m zeldaVR/gfx/*.c zeldaVR/gfx/*.m)
GFX_OBJECTS := $(addprefix $(OUT)/obj/,$(addsuffix .o,$(GFX_SRC)))
GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/*.inc third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h third_party/smaa/*.h zeldaVR/gfx/*.h zeldaVR/runtime/*.h)
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn/include -DGAME_OOT=1 -DGAME_NS2=1
.DEFAULT_GOAL := runtime
.PHONY: runtime
$(OUT)/obj/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"
$(OUT)/oot-native: $(wildcard runtime/*.[ch] runtime/*.m runtime/*.S) $(GFX_OBJECTS) Makefile runtime/game_info.plist app/launcher.m
	xcrun clang -std=c11 $(NATIVE_OPT) $(GAME_INFO) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -Ithird_party/d2r_gfx/gfx/nvn/include -DGAME_OOT=1 -DGAME_NS2=1 -I$(OUT) -I/opt/homebrew/opt/dav1d/include -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include runtime/loader.c runtime/game_data.c runtime/game_patches_ns2.c runtime/ns2_host.c runtime/ns2_codec.c runtime/hle_movie_av1.c runtime/ns2_audio_out.c runtime/ns2_opus_ms.c runtime/offline_host.c runtime/world_survey.c runtime/game_slot.c runtime/services.c runtime/romfs.c runtime/threads.c runtime/gate.S runtime/hle_stack.S runtime/hle_stack.c runtime/thread_enter.S runtime/gfx_bridge.c runtime/frontend.m runtime/account.c runtime/input.c runtime/hid_feedback.c runtime/motion_fusion.c runtime/autoload.c runtime/gamepad.m runtime/gamepad_feedback.m runtime/gfx_probe.c runtime/vpool_probe.m runtime/gpu_path_probe.m runtime/memory_probe.c runtime/shared_font.c runtime/ipc.c runtime/audio_renderer.c runtime/audio_mixer.c runtime/audio_reverb.c runtime/audio_effects.c runtime/fs_host.c runtime/opus_codec.c runtime/movie_host.c runtime/time_host.c runtime/applet_host.c runtime/swkbd_host.c app/launcher.m runtime/cloud_save.m $(GFX_OBJECTS) /opt/homebrew/opt/dav1d/lib/libdav1d.dylib /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a -framework VideoToolbox -framework CoreMedia -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -framework IOSurface -o "$(OUT)/oot-native"
runtime: $(OUT)/oot-native

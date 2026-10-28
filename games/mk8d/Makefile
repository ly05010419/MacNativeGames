# Mario Kart 8 Deluxe native host. Game files and generated outputs stay in the work folder, outside the checkout
# (scripts/build_mk8d.py --work); WORK is given relative to games/mk8d, without spaces (make cannot build into one).
ifeq ($(strip $(WORK)),)
$(error set WORK to the work folder, relative to games/mk8d (scripts/build_mk8d.py passes it))
endif
OUT := $(WORK)/build
PY ?= .venv/bin/python
NATIVE_OPT ?= -O2

GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(patsubst %,$(OUT)/obj/%.o,$(GFX_SRC))
GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h)
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -DGAME_MK8D=1 -Ithird_party/d2r_gfx/gfx/nvn/include
RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
HEADLESS_SRC := $(filter-out runtime/frontend_ios.m runtime/frontend.m runtime/gfx_bridge.c runtime/gfx_probe.c runtime/gpu_path_probe.m runtime/vpool_probe.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
RUNTIME_HEADERS := $(wildcard runtime/*.h)
GENERATED := $(OUT)/runtime_tables.h $(OUT)/romfs_config.h $(OUT)/shared_fonts_config.h

.PHONY: prepare runtime probe probe-headless clean
prepare:
	"$(PY)" scripts/prepare_runtime.py --work "$(WORK)"

$(GENERATED):
	@echo "missing $@: run make prepare" && false

$(OUT)/obj/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

$(OUT)/mk8d-native: $(RUNTIME_SRC) $(RUNTIME_HEADERS) $(GENERATED) $(GFX_OBJECTS) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations \
	  -Ithird_party/d2r_gfx/gfx/nvn/include -I$(OUT) -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include \
	  $(RUNTIME_SRC) $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a \
	  -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics \
	  -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation \
	  -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -framework VideoToolbox -framework CoreMedia -o "$@"

runtime: shader-tools $(OUT)/mk8d-native

# Play from the work folder (`make app` packages the app): saves, shader cache and the game slot go in $(WORK)/play. TOTK_INPUT_FILE
# takes diagnostic commands ("shot <png>", "key <code> <ms>"; runtime/input.c).
# DM_PRESENT_LINK_FALLBACK=1 (as Bayonetta 3's launcher): when macOS stops the display link for a window in front (seen after
# the display woke up), presents go on without it instead of the picture freezing; the link is tried again later.
# TOTK_WARM_SCOPE=observed (as the app): warm up only the shaders this work folder has used; with the whole-game shader seed in
# the cache, warming all of it would load about 30,000 libraries at every start. TOTK_WARM_SCOPE=all warms everything.
# DM_PRESENT_LINK=0: present each frame without the display link. Started from some shells (no active display list), the link
# went silent for 10-46 s at a time with the window in front; frame pacing of both modes is measured in phase 5.
PLAY = $(abspath $(WORK))/play
.PHONY: run
run: runtime
	mkdir -p "$(PLAY)/profile" "$(PLAY)/shader-cache" "$(PLAY)/slot" && : > "$(PLAY)/commands.txt"
	cd "$(WORK)" && TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 TOTK_SHADER_ASYNC=1 DM_PRESENT_LINK=0 DM_PRESENT_LINK_FALLBACK=1 TOTK_WARM_SCOPE=$${TOTK_WARM_SCOPE:-observed} \
	  TOTK_PROFILE_DIR="$(PLAY)/profile" TOTK_SHADER_CACHE="$(PLAY)/shader-cache/rt" TOTK_ROMFS="$(abspath $(WORK))/extracted/local/romfs" \
	  TOTK_SHARED_FONTS="$(abspath $(WORK))/extracted/firmware/shared-fonts" TOTK_INPUT_FILE="$(PLAY)/commands.txt" \
	  TOTK_GAME_SLOT_DIR="$(PLAY)/slot" TOTK_SLOT_OWNER=user TOTK_SLOT_PURPOSE=play TOTK_ROMFS_PATCHES=0 TOTK_ROMFS_PROOF_DIR="$(PLAY)" \
	  ./build/mk8d-native rewritten/local boot

# The app (scripts/package_mk8d_app.py): APFS clones of the work folder's runtime and game files, the launcher
# (scripts/mk8d_app_launcher.c: `make run`'s environment, full screen, logs and save backups) and the icon build_mk8d.py
# extracted. Saves and the shader cache go to DATA, seeded once from $(WORK)/play.
APP ?= $(abspath $(WORK))/Mario Kart 8 Deluxe.app
DATA ?= $(abspath $(WORK))/MK8D Data
.PHONY: app
app: runtime
	"$(PY)" scripts/package_mk8d_app.py --work "$(WORK)" --app "$(APP)" --data "$(DATA)"

probe: runtime
	cd "$(WORK)" && ./build/mk8d-native rewritten/local probe

$(OUT)/mk8d-headless: $(HEADLESS_SRC) headless/headless_stubs.c $(RUNTIME_HEADERS) $(GENERATED) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations \
	  -Ithird_party/d2r_gfx/gfx/nvn/include -I$(OUT) -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include \
	  $(HEADLESS_SRC) headless/headless_stubs.c /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a \
	  -fobjc-arc -framework Foundation -framework Cocoa -framework CoreVideo \
	  -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -framework VideoToolbox -framework CoreMedia -o "$@"

probe-headless: $(OUT)/mk8d-headless
	cd "$(WORK)" && ./build/mk8d-headless rewritten/local probe

# Shader translation uses the reference project's pinned SPIRV-Cross commit.
# Fetch it into $(WORK)/src/SPIRV-Cross before building this target.
SPIRV_CROSS_COMMIT := aa217aeb6c9f0ace7a0ab233b28807edf45eb165
SPIRV_CROSS_SOURCES := main.cpp spirv_cfg.cpp spirv_cpp.cpp spirv_cross.cpp spirv_cross_parsed_ir.cpp \
  spirv_cross_util.cpp spirv_glsl.cpp spirv_hlsl.cpp spirv_msl.cpp spirv_parser.cpp spirv_reflect.cpp
SPIRV_CROSS_OBJECTS := $(addprefix $(OUT)/spirv-cross/obj/,$(addsuffix .o,$(SPIRV_CROSS_SOURCES)))
.PHONY: shader-tools
shader-tools: $(OUT)/shader-service/totk_xlate $(OUT)/spirv-cross/spirv-cross
$(OUT)/shader-service/totk_xlate: prebuilt/macos-arm64/totk_xlate prebuilt/macos-arm64/totk_xlate.sha256
	@mkdir -p "$(dir $@)"
	test "$$(shasum -a 256 "$<" | cut -d' ' -f1)" = "$$(cut -d' ' -f1 prebuilt/macos-arm64/totk_xlate.sha256)"
	cp "$<" "$@" && chmod +x "$@"
$(OUT)/spirv-cross/obj/%.cpp.o: $(WORK)/src/SPIRV-Cross/%.cpp Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang++ -O3 -DNDEBUG -std=c++11 -arch arm64 -Wno-deprecated-declarations -c "$<" -o "$@"
$(OUT)/spirv-cross/spirv-cross: $(SPIRV_CROSS_OBJECTS)
	xcrun clang++ -arch arm64 $(SPIRV_CROSS_OBJECTS) -o "$@"

clean:
	rm -rf "$(OUT)/obj" "$(OUT)/mk8d-native"

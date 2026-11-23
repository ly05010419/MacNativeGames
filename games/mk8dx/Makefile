# Mario Kart 8 Deluxe 4.0.1 native host on the Zelda-line runtime (forked from games/odyssey, third_party/COPIES.md).
# Game files and generated outputs stay in the work folder (WORK, outside the checkout).
# opus and libvpx come from the repository's tools/deps: made once per Mac from pinned sources (no Homebrew).
# make runs here (games/<game>); set a *_PREFIX to use another build.
OPUS_PREFIX ?= $(or $(shell python3 ../../tools/deps prefix opus),$(error tools/deps could not make opus))
VPX_PREFIX ?= $(or $(shell python3 ../../tools/deps prefix libvpx),$(error tools/deps could not make libvpx))

WORK ?= $(error Set WORK to the work folder (README.md))
OUT := $(WORK)/build
PY ?= .venv/bin/python
NATIVE_OPT ?= -O2
# The host's bundle id (runtime/game_info.plist), as Tears of the Kingdom's: Metal's archive builder then works in a cache
# folder of its own, which the pipeline archive needs to write (totk_pipeline_archive_folders.h; the pipeline-archive mode).
GAME_INFO := -Wl,-sectcreate,__TEXT,__info_plist,runtime/game_info.plist

GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(patsubst %,$(OUT)/obj/%.o,$(GFX_SRC))
GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/*.inc third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h)
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -DGAME_MK8DX=1 -Ithird_party/d2r_gfx/gfx/nvn/include
RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
RUNTIME_HEADERS := $(wildcard runtime/*.h)
GENERATED := $(OUT)/runtime_tables.h $(OUT)/romfs_config.h $(OUT)/shared_fonts_config.h

.PHONY: prepare runtime probe boot clean
prepare:
	"$(PY)" scripts/prepare_runtime.py --work "$(WORK)"

$(GENERATED):
	@echo "missing $@: run make prepare" && false

$(OUT)/obj/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

$(OUT)/mk8dx-native: $(RUNTIME_SRC) $(RUNTIME_HEADERS) $(GENERATED) $(GFX_OBJECTS) runtime/game_info.plist Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) $(GAME_INFO) -g -Wall -Wextra -Werror -Wno-deprecated-declarations \
	  -Ithird_party/d2r_gfx/gfx/nvn/include -I$(OUT) -I$(OPUS_PREFIX)/include/opus -I$(VPX_PREFIX)/include \
	  $(RUNTIME_SRC) $(GFX_OBJECTS) $(OPUS_PREFIX)/lib/libopus.a $(VPX_PREFIX)/lib/libvpx.a \
	  -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics \
	  -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation \
	  -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -framework VideoToolbox -framework CoreMedia -o "$@"

runtime: shader-tools $(OUT)/mk8dx-native

probe: runtime
	cd "$(WORK)" && ./build/mk8dx-native rewritten/local probe

boot: runtime
	cd "$(WORK)" && ./build/mk8dx-native rewritten/local boot

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
	rm -rf "$(OUT)/obj" "$(OUT)/mk8dx-native"

# Mario vs. Donkey Kong 1.0.1 native runtime, forked from games/wonder (the same sead engine). Self-contained: the Horizon host
# (runtime/), the rewriter (third_party/d2r), the NVN-on-Metal layer (third_party/d2r_gfx), the shader translator (prebuilt/)
# and the scripts are this folder's own copies; nothing here reads another game's folder. scripts/build_mvdk.py copies this
# folder into the work folder (<work>/mvdk) and runs make there, so extracted/, rewritten/, analysis/ and build/ sit next to
# the sources.
#   make runtime   # build/mvdk-native, from the tables of scripts/prepare_runtime.py and the RomFS / font configs
#   make boot      # run it from this folder
# GAME_WONDER is defined too: the host's Wonder paths (offline services, 60 Hz VI vsync) apply to this game as well.
VERSION ?= mvdk101

GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(addprefix build/gfx/,$(addsuffix .o,$(GFX_SRC)))
GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h)
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn/include
NATIVE_OPT ?= -O2
PY := .venv/bin/python
VERSION_FLAGS := $(if $(filter mvdk101,$(VERSION)),,$(error VERSION must be mvdk101))
BINARY := build/mvdk-native

RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
RUNTIME_HEADERS := $(wildcard runtime/*.h)

build/gfx/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

.PHONY: runtime boot clean
build/$(VERSION)/runtime_tables.h: scripts/prepare_runtime.py analysis/$(VERSION)-port/phase12-validation.json
	"$(PY)" scripts/prepare_runtime.py --version $(VERSION)
build/$(VERSION)/romfs_config.h: scripts/prepare_romfs_backend.py extracted/$(VERSION)/romfs/index.json
	"$(PY)" scripts/prepare_romfs_backend.py --version $(VERSION)
build/shared_fonts_config.h: scripts/prepare_shared_fonts.py extracted/firmware/shared-fonts/manifest.json
	"$(PY)" scripts/prepare_shared_fonts.py

$(BINARY): $(RUNTIME_SRC) $(RUNTIME_HEADERS) build/$(VERSION)/runtime_tables.h build/$(VERSION)/romfs_config.h build/shared_fonts_config.h $(GFX_OBJECTS) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -Ithird_party/d2r_gfx/gfx/nvn/include -DGAME_MVDK=1 -DGAME_WONDER=1 $(VERSION_FLAGS) -Iruntime -Ibuild/$(VERSION) -Ibuild -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include $(RUNTIME_SRC) $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework VideoToolbox -framework CoreMedia -framework GameController -framework CoreHaptics -framework IOKit -o "$@"
runtime: $(BINARY)

boot: runtime
	$(BINARY) rewritten/$(VERSION) boot

clean:
	rm -rf build/gfx build/mvdk-native

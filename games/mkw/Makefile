# Build in the external staged source copy made by scripts/build_mkw.py.
ifneq ($(wildcard ../../.git),)
$(error Use scripts/build_mkw.py --work outside the checkout; direct builds write next to these sources)
endif
# Mario Kart World 1.0 native runtime (the Switch 2 game). Self-contained: the Horizon host (runtime/), the rewriter
# (third_party/d2r), the NVN-on-Metal layer (third_party/d2r_gfx), and the scripts are
# this folder's own copies, forked from games/wonder (itself forked from games/totk); nothing here reads another game's
# folder. The one-command build is not adapted yet; extracted/, rewritten/, analysis/ and build/ sit next to the sources.
#   make runtime   # build/mkw-native (make binary) and the game data folder build/game-data/<VERSION> it reads at start
#                  # (make game-data: the tables of scripts/prepare_runtime.py and the RomFS / font configs)
#   make boot      # run it from this folder
# VERSION: mkw (1.0.0), the folders under extracted/, rewritten/ and build/ (runtime/game_version.h TOTK_GAME_DIR).
VERSION ?= mkw

GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(addprefix build/gfx/,$(addsuffix .o,$(GFX_SRC)))
GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h)
GFX_FLAGS := -Ibuild -mmacosx-version-min=26.0 -std=gnu11 -DGAME_MKW=1 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn/include
NATIVE_OPT ?= -O2
PY := .venv/bin/python
VERSION_FLAGS := $(if $(filter mkw,$(VERSION)),,$(error VERSION must be mkw))
BINARY := build/mkw-native

RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
RUNTIME_HEADERS := $(wildcard runtime/*.h)

build/gfx/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

.PHONY: runtime binary game-data shader-tools boot clean
# The game data folder (runtime/game_data.h): what the runtime reads from the player's own files at start (module tables,
# RomFS / shared-font sizes and hashes), not compiled into the binary.
GAME_DATA := build/game-data/$(VERSION)
$(GAME_DATA)/runtime-tables.bin: scripts/prepare_runtime.py analysis/$(VERSION)-port/phase12-validation.json
	"$(PY)" scripts/prepare_runtime.py --version $(VERSION)
$(GAME_DATA)/rewriter.txt: $(GAME_DATA)/runtime-tables.bin
	@test -f $@ || "$(PY)" scripts/prepare_runtime.py --version $(VERSION)
$(GAME_DATA)/romfs-config.txt: scripts/prepare_romfs_backend.py extracted/$(VERSION)/romfs/index.json
	"$(PY)" scripts/prepare_romfs_backend.py --version $(VERSION)
$(GAME_DATA)/shared-fonts.txt: scripts/prepare_shared_fonts.py extracted/firmware/shared-fonts/manifest.json
	"$(PY)" scripts/prepare_shared_fonts.py --version $(VERSION)
game-data: $(GAME_DATA)/runtime-tables.bin $(GAME_DATA)/rewriter.txt $(GAME_DATA)/romfs-config.txt $(GAME_DATA)/shared-fonts.txt

$(BINARY): $(RUNTIME_SRC) $(RUNTIME_HEADERS) $(GFX_OBJECTS) Makefile
	xcrun clang -mmacosx-version-min=26.0 -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -Ithird_party/d2r_gfx/gfx/nvn/include -DGAME_MKW=1 $(VERSION_FLAGS) -Iruntime -Ibuild -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include $(RUNTIME_SRC) $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@"
binary: $(BINARY)
# The shader translation chain from pinned sources (scripts/build_shader_tools.py): SPIRV-Cross, the vendored nxnative
# SM86 frontend (third_party/nxnative) and tools/ampere_service, at the fixed paths the runtime starts them from.
shader-tools:
	python3 scripts/build_shader_tools.py
runtime: binary game-data shader-tools

boot: runtime
	$(BINARY) rewritten/$(VERSION) boot

clean:
	rm -rf build/gfx build/mkw-native

# The executable and shader cache must describe the same verified tool chain.
$(GFX_OBJECTS) $(BINARY): build/mkw_shader_chain.h
build/mkw_shader_chain.h: shader-tools
	@test -f "$@"

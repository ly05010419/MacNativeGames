# Super Mario Bros. Wonder 1.0.0 native runtime. Self-contained: the Horizon host (runtime/), the rewriter (third_party/d2r),
# the NVN-on-Metal layer (third_party/d2r_gfx), the shader translator (prebuilt/) and the scripts are this folder's own
# copies, forked from games/totk; nothing here reads games/totk. scripts/build_wonder.py copies this folder into the work
# folder (<work>/wonder) and runs make there, so extracted/, rewritten/, analysis/ and build/ sit next to the sources.
#   make runtime   # build/wonder-native (make binary) and the game data folder build/game-data/<VERSION> it reads at start
#                  # (make game-data: the tables of scripts/prepare_runtime.py and the RomFS / font configs)
#   make boot      # run it from this folder
# VERSION: wonder (1.0.0, the base game) or wonder121 (the 1.2.1 update): the folders under extracted/, rewritten/ and
# build/ (runtime/game_version.h TOTK_GAME_DIR); the binary is build/wonder-native for 1.0.0, build/wonder-native-<VERSION> else.
VERSION ?= wonder

GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(addprefix build/gfx/,$(addsuffix .o,$(GFX_SRC)))
GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h)
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn/include
NATIVE_OPT ?= -O2
PY := .venv/bin/python
VERSION_FLAGS := $(if $(filter wonder121,$(VERSION)),-DWONDER_V121=1,$(if $(filter wonder,$(VERSION)),,$(error VERSION must be wonder or wonder121)))
BINARY := build/wonder-native$(if $(filter wonder,$(VERSION)),,-$(VERSION))

RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
RUNTIME_HEADERS := $(wildcard runtime/*.h)

build/gfx/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

.PHONY: runtime binary game-data boot clean
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
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -Ithird_party/d2r_gfx/gfx/nvn/include -DGAME_WONDER=1 $(VERSION_FLAGS) -Iruntime -Ibuild -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include $(RUNTIME_SRC) $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@"
binary: $(BINARY)
runtime: binary game-data

boot: runtime
	$(BINARY) rewritten/$(VERSION) boot

clean:
	rm -rf build/gfx build/wonder-native build/wonder-native-*

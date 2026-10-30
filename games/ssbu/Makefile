# Super Smash Bros. Ultimate native runtime. Self-contained: the Horizon host (runtime/), the rewriter (third_party/d2r), the
# NVN-on-Metal layer (third_party/d2r_gfx) and the shader translator (prebuilt/) are this folder's own copies, forked from
# games/totk with the SDK 7.3.2 branches Breath of the Wild uses (CHANGELOG.md, stage 2); nothing here reads games/totk.
# Everything generated goes to the work folder.
# Inputs come from scripts/extract_code.py, scripts/merge_romfs.py and the rewrite (see README.md), then:
#   make prepare   # scripts/prepare_runtime.py: tables, RomFS index, fonts config
#   make runtime   # SSBU_build/build/ssbu-native
#   make boot      # run it from the work folder (cwd: extracted/ and rewritten/ are read relative to it)

WORK ?= SSBU_build
OUT := $(WORK)/build
PY ?= $(WORK)/venv/bin/python
NATIVE_OPT ?= -O2
SHARED_FONTS ?=
# The game version (scripts/build_ssbu.py chooses it from the update's verified ExeFS): v1302 or v1305. The runtime is built
# for one (runtime/game_version.h: 13.0.2 without a define); its stamp makes a change of version compile it again.
VERSION ?= v1302
ifeq ($(filter $(VERSION),v1302 v1305),)
$(error VERSION must be v1302 or v1305)
endif
VERSION_FLAGS := $(if $(filter v1305,$(VERSION)),-DSSBU_GAME_V1305=1)

GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(patsubst %,$(OUT)/obj/%.o,$(GFX_SRC))
GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h)
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn/include

RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
RUNTIME_HEADERS := $(wildcard runtime/*.h)
GENERATED := $(OUT)/runtime_tables.h $(OUT)/romfs_config.h $(OUT)/shared_fonts_config.h

.PHONY: prepare runtime boot clean
prepare:
	"$(PY)" scripts/prepare_runtime.py --work "$(WORK)" --version $(VERSION) $(if $(SHARED_FONTS),--shared-fonts "$(SHARED_FONTS)")

$(GENERATED):
	@echo "missing $@: run make prepare" && false

$(OUT)/obj/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

$(OUT)/version-$(VERSION):
	@mkdir -p "$(OUT)" && rm -f "$(OUT)"/version-v* && touch "$@"

$(OUT)/ssbu-native: $(RUNTIME_SRC) $(RUNTIME_HEADERS) $(GENERATED) $(GFX_OBJECTS) $(OUT)/version-$(VERSION) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations $(VERSION_FLAGS) \
	  -Ithird_party/d2r_gfx/gfx/nvn/include -I$(OUT) -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include -I/opt/homebrew/opt/libvorbis/include -I/opt/homebrew/opt/libogg/include \
	  $(RUNTIME_SRC) $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a /opt/homebrew/opt/libvorbis/lib/libvorbis.a /opt/homebrew/opt/libogg/lib/libogg.a \
	  -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics \
	  -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation \
	  -framework VideoToolbox -framework CoreMedia \
	  -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@.new"
	mv -f "$@.new" "$@"   # a new file, not new bytes in the old one: a game started from it keeps running

runtime: $(OUT)/ssbu-native

# The runtime shader translation the graphics layer starts on first use: <work>/build/shader-service/totk_xlate (the
# repository's prebuilt NativeAOT translator, checked against its SHA-256) and <work>/build/spirv-cross/spirv-cross,
# built like build_app.py does (same pinned commit, sources and flags as SPIRV-Cross's CMake Release build).
SPIRV_CROSS_COMMIT := aa217aeb6c9f0ace7a0ab233b28807edf45eb165
SPIRV_CROSS_SOURCES := main.cpp spirv_cfg.cpp spirv_cpp.cpp spirv_cross.cpp spirv_cross_parsed_ir.cpp spirv_cross_util.cpp \
  spirv_glsl.cpp spirv_hlsl.cpp spirv_msl.cpp spirv_parser.cpp spirv_reflect.cpp
.PHONY: shader-tools
shader-tools: $(OUT)/shader-service/totk_xlate $(OUT)/spirv-cross/spirv-cross
$(OUT)/shader-service/totk_xlate: prebuilt/macos-arm64/totk_xlate prebuilt/macos-arm64/totk_xlate.sha256
	@mkdir -p "$(dir $@)"
	test "$$(shasum -a 256 "$<" | cut -d' ' -f1)" = "$$(cut -d' ' -f1 "prebuilt/macos-arm64/totk_xlate.sha256")"
	cp "$<" "$@.new" && chmod +x "$@.new" && mv -f "$@.new" "$@"   # as above: a running translator keeps its file
$(OUT)/spirv-cross/spirv-cross:
	@mkdir -p "$(WORK)/src" "$(dir $@)obj"
	test -d "$(WORK)/src/SPIRV-Cross" || git clone -q https://github.com/KhronosGroup/SPIRV-Cross.git "$(WORK)/src/SPIRV-Cross"
	git -C "$(WORK)/src/SPIRV-Cross" checkout -q $(SPIRV_CROSS_COMMIT)
	for f in $(SPIRV_CROSS_SOURCES); do xcrun clang++ -O3 -DNDEBUG -std=c++11 -arch arm64 -Wno-deprecated-declarations \
	  -c "$(WORK)/src/SPIRV-Cross/$$f" -o "$(dir $@)obj/$$f.o" & done; wait
	xcrun clang++ -arch arm64 $(addprefix $(dir $@)obj/,$(addsuffix .o,$(SPIRV_CROSS_SOURCES))) -o "$@"

# The startup shader seed (scripts/prepare_shader_seed.py, game ssbu): every shader in data.arc (the BNSH files of the
# effects and the SSBH shader files) for its default bind state, plus every variant the shared list
# (assets/shader_list.tsv) names, translated from the player's own RomFS into <work>/build/shader-seed, which the runtime
# imports at the next start (totk_shader_rt.m seed_dir), so a new stage or fighter does not translate while it draws
# (Q55). Game-derived: it stays in the work folder.
.PHONY: seed shader-list
seed: shader-tools
	"$(PY)" scripts/prepare_shader_seed.py --game ssbu --romfs "$(WORK)/extracted/$(VERSION)/romfs" --output "$(OUT)/shader-seed" \
	  --translator "$(OUT)/shader-service/totk_xlate" --spirv-cross "$(OUT)/spirv-cross/spirv-cross" --version $(VERSION) \
	  --modules "$(WORK)/rewritten/$(VERSION)"
# The game's shaders compiled into Metal libraries ahead of time (scripts/build_shader_libs.py; build_app.py step 3/4):
# <work>/build/shader-libs/<SHA-256 of the source>.metallib, next to the runtime, which loads an exact match instead of
# compiling the source while the game draws. The sources are the seed (when `make seed` made one: every shader of
# data.arc, which the start's background warm-up loads) and the runtime's own translation cache: every shader it
# translated (<key>_b.metal) and every patched variant it compiled (variants/). SHADER_CACHE: another cache (an app's, in
# ~/Library/Application Support/MacNativeGames/<game>/shader-cache/rt-<chain>); SHADER_LIBS: the folder next to the
# executable that runs (an app's Contents/Resources/<...>/build/shader-libs). A rerun compiles only what is new.
SHADER_CACHE ?= $(OUT)/shader-cache/rt-$(CHAIN)
SHADER_LIBS ?= $(OUT)/shader-libs
SHADER_SEED ?= $(OUT)/shader-seed
.PHONY: shader-libs
shader-libs:
	"$(PY)" scripts/build_shader_libs.py $(if $(wildcard $(SHADER_SEED)/seed-manifest.json),--seed "$(SHADER_SEED)") \
	  --cache "$(SHADER_CACHE)" --out "$(SHADER_LIBS)"
# Add the shaders met in play to the shared list (identifiers only; send it in a pull request, docs/shader-list.md).
# The cache folder is named by the translation chain in dmsh_chain.h (a fixed name here went stale when it moved to v7).
CHAIN := $(shell sed -n 's/.*DMSH_CHAIN "\(.*\)".*/\1/p' third_party/d2r_gfx/gfx/nvn/dmsh_chain.h)
shader-list:
	"$(PY)" scripts/shader_list.py merge assets/shader_list.tsv "$(OUT)/shader-cache/rt-$(CHAIN)"

# Uniform textures (tests/uniform_texture/probe.m): the reflection gives a texture whose handle the shader loads from a
# user uniform block its slot and refuses the forms the runtime cannot bind. UTEX_JSON=<key>_b.json also loads a real one.
.PHONY: test-utex
test-utex:
	@mkdir -p "$(OUT)"
	xcrun clang -fobjc-arc -O1 -Wall -Wextra -Werror -Ithird_party/d2r_gfx/gfx/nvn tests/uniform_texture/probe.m \
	  third_party/d2r_gfx/gfx/nvn/nvn_refl.m -framework Metal -framework Foundation -o "$(OUT)/uniform-texture-probe"
	"$(OUT)/uniform-texture-probe" $(UTEX_JSON)

# SSBU_LOG: where the boot's stderr goes (the host is loud on purpose; every abort names what is missing).
SSBU_LOG ?= $(WORK)/logs/boot.log
boot: runtime
	@mkdir -p "$(dir $(SSBU_LOG))"
	cd "$(WORK)" && TOTK_GPU_ALLOW_SUBMIT=$${TOTK_GPU_ALLOW_SUBMIT-1} TOTK_HEADLESS=$${TOTK_HEADLESS-} ./build/ssbu-native rewritten/$(VERSION) boot 2> "$(abspath $(SSBU_LOG))"; echo "exit $$?; log: $(SSBU_LOG)"

clean:
	rm -rf "$(OUT)/obj" "$(OUT)/ssbu-native"

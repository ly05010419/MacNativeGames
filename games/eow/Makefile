# The Legend of Zelda: Echoes of Wisdom native runtime (work in progress). Self-contained: the Horizon host (runtime/), the
# rewriter (third_party/d2r), the NVN-on-Metal layer (third_party/d2r_gfx), the shader translator (prebuilt/) and the
# scripts are this folder's own copies, forked from games/by3 (itself forked from games/totk); nothing here reads another
# game's folder. Everything generated goes to the work folder.
# Inputs come from scripts/extract_code.py, scripts/merge_romfs.py and the rewrite (see README.md), then:
#   make prepare   # scripts/prepare_runtime.py: tables, RomFS index, fonts config
#   make runtime   # EoW_build/build/eow-native
#   make boot      # run it from the work folder (cwd: extracted/ and rewritten/ are read relative to it)

# opus and libvpx come from the repository's tools/deps: made once per Mac from pinned sources (no Homebrew).
# make runs here (games/<game>); set a *_PREFIX to use another build.
OPUS_PREFIX ?= $(or $(shell python3 ../../tools/deps prefix opus),$(error tools/deps could not make opus))
VPX_PREFIX ?= $(or $(shell python3 ../../tools/deps prefix libvpx),$(error tools/deps could not make libvpx))

WORK ?= EoW_build
OUT := $(WORK)/build
PY ?= python3
NATIVE_OPT ?= -O2

GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(patsubst %,$(OUT)/obj/%.o,$(GFX_SRC))
GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h)
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn/include

RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
RUNTIME_HEADERS := $(wildcard runtime/*.h)
GENERATED := $(OUT)/runtime_tables.h $(OUT)/romfs_config.h $(OUT)/shared_fonts_config.h

.PHONY: prepare runtime boot clean
prepare:
	"$(PY)" scripts/prepare_runtime.py --work "$(WORK)"

$(GENERATED):
	@echo "missing $@: run make prepare" && false

$(OUT)/obj/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

$(OUT)/eow-native: $(RUNTIME_SRC) $(RUNTIME_HEADERS) $(GENERATED) $(GFX_OBJECTS) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations \
	  -Ithird_party/d2r_gfx/gfx/nvn/include -I$(OUT) -I$(OPUS_PREFIX)/include/opus -I$(VPX_PREFIX)/include \
	  $(RUNTIME_SRC) $(GFX_OBJECTS) $(OPUS_PREFIX)/lib/libopus.a $(VPX_PREFIX)/lib/libvpx.a \
	  -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics \
	  -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation \
	  -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@"

runtime: $(OUT)/eow-native

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
	cp "$<" "$@" && chmod +x "$@"
$(OUT)/spirv-cross/spirv-cross:
	@mkdir -p "$(WORK)/src" "$(dir $@)obj"
	test -d "$(WORK)/src/SPIRV-Cross" || git clone -q https://github.com/KhronosGroup/SPIRV-Cross.git "$(WORK)/src/SPIRV-Cross"
	git -C "$(WORK)/src/SPIRV-Cross" checkout -q $(SPIRV_CROSS_COMMIT)
	for f in $(SPIRV_CROSS_SOURCES); do xcrun clang++ -O3 -DNDEBUG -std=c++11 -arch arm64 -Wno-deprecated-declarations \
	  -c "$(WORK)/src/SPIRV-Cross/$$f" -o "$(dir $@)obj/$$f.o" & done; wait
	xcrun clang++ -arch arm64 $(addprefix $(dir $@)obj/,$(addsuffix .o,$(SPIRV_CROSS_SOURCES))) -o "$@"

# The prepared shaders, from the player's own game files: every shader translated (<work>/build/shader-seed, which the
# runtime imports at its next start) and compiled (<work>/build/shader-libs), published together only when both are
# complete (scripts/prepare_shader_seed.py). The Metal compiler comes from Xcode (DEVELOPER_DIR).
.PHONY: shader-seed test
shader-seed: shader-tools
	"$(PY)" scripts/prepare_shader_seed.py --work "$(WORK)"

# The tests (no game data, no Metal compiler): the scripts', and the movie decoder's Stop with a frame still held
# (tests/movie_stop, AddressSanitizer; the crash when the intro movie stops).
test: $(OUT)/test-movie-stop
	"$(PY)" -m unittest discover -s tests
	"$(OUT)/test-movie-stop"
$(OUT)/test-movie-stop: tests/movie_stop/test.c runtime/movie_host.c runtime/movie_host.h Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
	  -I$(VPX_PREFIX)/include -Ithird_party/d2r_gfx/gfx/nvn/include \
	  runtime/movie_host.c tests/movie_stop/test.c $(VPX_PREFIX)/lib/libvpx.a -o "$@"

# EOW_LOG: where the boot's stderr goes (the host is loud on purpose; every abort names what is missing).
EOW_LOG ?= $(WORK)/logs/boot.log
boot: runtime
	@mkdir -p "$(dir $(EOW_LOG))"
	@if [ -f "$(WORK)/build/shader-seed/seed-manifest.json" ]; then \
	  "$(PY)" scripts/build_shader_libs.py --work "$(WORK)" --check || \
	  echo "warning: the shader libraries do not serve the prepared seed (make shader-seed); play compiles the rest"; fi
	cd "$(WORK)" && TOTK_HEADLESS=$${TOTK_HEADLESS-} ./build/eow-native rewritten/v110 boot 2> "$(abspath $(EOW_LOG))"; echo "exit $$?; log: $(EOW_LOG)"

# The player's mods, made into checked tables: exefs patches (mods/1.1.0/<name>/exefs/*.pchtxt) -> <work>/patches,
# RomFS mods (mods/<name>/romfs/...) -> their manifest.tsv (sizes and SHA-256 of the replacements and the originals).
.PHONY: patches mods
patches:
	"$(PY)" scripts/prepare_patches.py --work "$(WORK)"
mods:
	for d in mods/*/romfs; do [ -d "$$d" ] && "$(PY)" scripts/make_mod_manifest.py "$$d" --romfs "$(WORK)/extracted/v110/romfs"; done; true

clean:
	rm -rf "$(OUT)/obj" "$(OUT)/eow-native"

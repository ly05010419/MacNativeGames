# Pokémon Legends: Arceus native runtime. Self-contained: the Horizon host (runtime/), the rewriter
# (third_party/d2r), the NVN-on-Metal layer (third_party/d2r_gfx), the shader translator (prebuilt/) and the
# scripts are this folder's own copies, forked from games/by3; nothing here reads games/by3 or games/totk.
# Everything generated goes to the work folder.
# Inputs come from scripts/extract_exefs.py, scripts/extract_romfs.py + scripts/merge_romfs.py and the rewrite
# (see README.md), then:
#   make prepare   # scripts/prepare_runtime.py: tables, RomFS index, fonts config
#   make runtime   # PLA_build/build/pla-native
#   make boot      # run it from the work folder (cwd: extracted/ and rewritten/ are read relative to it)

WORK ?= $(error Set WORK to an absolute directory outside the source checkout)
OUT := $(WORK)/build
PY ?= python3
NATIVE_OPT ?= -O2
MACOS_MIN := -mmacosx-version-min=26.0

GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(patsubst %,$(OUT)/obj/%.o,$(GFX_SRC))
GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/*.inc third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h)
SCENECAP ?= 0
GFX_FLAGS := -std=gnu11 -DSCENECAP_HOOKS=$(SCENECAP) -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn/include

ifeq ($(SCENECAP),0)
GFX_SRC := $(filter-out %/scenecap_capture.c %/nvs_format.c,$(GFX_SRC))
GFX_OBJECTS := $(patsubst %,$(OUT)/obj/%.o,$(GFX_SRC))
endif

RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
RUNTIME_HEADERS := $(wildcard runtime/*.h)
GENERATED := $(OUT)/runtime_tables.h $(OUT)/romfs_config.h $(OUT)/shared_fonts_config.h

.PHONY: prepare runtime boot clean app dist
prepare: check-paths
	"$(PY)" scripts/prepare_runtime.py --work "$(WORK)"

$(GENERATED):
	@echo "missing $@: run make prepare" && false

$(OUT)/obj/%.o: % $(GFX_HEADERS) Makefile | check-paths
	@mkdir -p "$(dir $@)"
	xcrun clang $(MACOS_MIN) $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

$(OUT)/pla-native: $(RUNTIME_SRC) $(RUNTIME_HEADERS) $(GENERATED) $(GFX_OBJECTS) Makefile
	xcrun clang $(MACOS_MIN) -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations \
	  -DSCENECAP_HOOKS=$(SCENECAP) \
	  -Ithird_party/d2r_gfx/gfx/nvn/include -I$(OUT) -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include \
	  $(RUNTIME_SRC) $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a \
	  -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics \
	  -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation \
	  -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@"

# Shader seed, the same pipeline as Tears of the Kingdom: the shared shader list -> pso_manifest.tsv, which the Metal
# layer reads to build every pipeline before the first frame (nvn_mtl.m: pso_manifest.tsv next to the shader map).
SHADER_LIST ?= assets/shader_list.tsv
$(OUT)/pso_manifest.tsv: $(SHADER_LIST) scripts/shader_list.py
	@mkdir -p "$(OUT)"
	"$(PY)" scripts/shader_list.py export "$(SHADER_LIST)" "$(OUT)/pso_manifest.tsv" "$(OUT)/pso_title.tsv"

runtime: $(OUT)/pso_manifest.tsv
runtime: $(OUT)/pla-native

# The runtime shader translation the graphics layer starts on first use: <work>/build/shader-service/totk_xlate (the
# repository's prebuilt NativeAOT translator, checked against its SHA-256) and <work>/build/spirv-cross/spirv-cross,
# built like build_app.py does (same pinned commit, sources and flags as SPIRV-Cross's CMake Release build).
SPIRV_CROSS_COMMIT := aa217aeb6c9f0ace7a0ab233b28807edf45eb165
SPIRV_CROSS_SOURCES := main.cpp spirv_cfg.cpp spirv_cpp.cpp spirv_cross.cpp spirv_cross_parsed_ir.cpp spirv_cross_util.cpp \
  spirv_glsl.cpp spirv_hlsl.cpp spirv_msl.cpp spirv_parser.cpp spirv_reflect.cpp
.PHONY: shader-tools
shader-tools: $(OUT)/shader-service/totk_xlate $(OUT)/spirv-cross/spirv-cross
$(OUT)/shader-service/totk_xlate: prebuilt/macos-arm64/totk_xlate prebuilt/macos-arm64/totk_xlate.sha256 | check-paths
	@mkdir -p "$(dir $@)"
	test "$$(shasum -a 256 "$<" | cut -d' ' -f1)" = "$$(cut -d' ' -f1 "prebuilt/macos-arm64/totk_xlate.sha256")"
	cp "$<" "$@" && chmod +x "$@"
$(OUT)/spirv-cross/spirv-cross: | check-paths
	@mkdir -p "$(WORK)/src" "$(dir $@)obj"
	test -d "$(WORK)/src/SPIRV-Cross" || git clone -q https://github.com/KhronosGroup/SPIRV-Cross.git "$(WORK)/src/SPIRV-Cross"
	git -C "$(WORK)/src/SPIRV-Cross" checkout -q $(SPIRV_CROSS_COMMIT)
	for f in $(SPIRV_CROSS_SOURCES); do xcrun clang++ -O3 -DNDEBUG -std=c++11 -arch arm64 -Wno-deprecated-declarations \
	  -c "$(WORK)/src/SPIRV-Cross/$$f" -o "$(dir $@)obj/$$f.o" & done; wait
	xcrun clang++ -arch arm64 $(addprefix $(dir $@)obj/,$(addsuffix .o,$(SPIRV_CROSS_SOURCES))) -o "$@"

# PLA_LOG: where the boot's stderr goes (the host is loud on purpose; every abort names what is missing).
PLA_LOG ?= $(WORK)/logs/boot.log
# TOTK_FREE_CORES=1: the guest has user-mode spin loops with no SVC inside; without it a worker starves [F1].
# TOTK_GPU_MIDCLEAR=1: this game clears mid-pass masked+scissored on normal boot [C3]. TOTK_GPU_ALLOW_SUBMIT=1
# only once the first frames have been replayed with submits off.
boot: runtime
	@mkdir -p "$(dir $(PLA_LOG))"
	cd "$(WORK)" && ./build/pla-native rewritten/v1101 boot 2> "$(abspath $(PLA_LOG))"; echo "exit $$?; log: $(PLA_LOG)"


# The player's mods, made into checked tables: exefs patches (mods/1.1.1/<name>/exefs/*.pchtxt) -> <work>/patches,
# RomFS mods (mods/<name>/romfs/...) -> their manifest.tsv (sizes and SHA-256 of the replacements and the originals).
.PHONY: patches mods app
patches: check-paths
	"$(PY)" scripts/prepare_patches.py --work "$(WORK)"
mods:
	for d in mods/*/romfs; do [ -d "$$d" ] && "$(PY)" scripts/make_mod_manifest.py "$$d" --romfs "$(WORK)/extracted/v1101/romfs"; done; true

# The app: a small launcher bundle (settings window with presets, then the game) that runs the game from the work
# folder, which it names in Info.plist (PLAWorkFolder): nothing large is copied. Its icon comes from the game's own
# control data (GAME=<card image> KEYS=<prod.keys>), or the app goes without one.
APP ?= $(abspath $(WORK)/..)/Pokemon Legends Arceus.app
GAME ?=
KEYS ?=
$(OUT)/launcher: app/launcher.m Makefile | check-paths
	xcrun clang -std=gnu11 -O2 -Wall -Wextra -Werror -fobjc-arc app/launcher.m $(MACOS_MIN) -framework Cocoa -o "$@"
$(OUT)/AppIcon.icns: | check-paths
	@if [ -n "$(GAME)" ] && [ -n "$(KEYS)" ]; then cd scripts && "$(PY)" extract_app_icon.py "$(GAME)" --keys "$(KEYS)" --title 01001f5010dfa000 --output "$(abspath $@)"; \
	 else echo "no GAME= / KEYS=: the app gets no icon"; fi
# One command for a distributable build: runtime + shader seed + the self-contained app bundle.
dist: runtime app
	@echo "dist ready: $(APP) (self-contained; shader seed $(OUT)/pso_manifest.tsv)"

app: check-new-app runtime shader-tools $(OUT)/launcher $(OUT)/AppIcon.icns
	rm -rf "$(APP)" && mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources"
	cp "$(OUT)/launcher" "$(APP)/Contents/MacOS/Pokemon Legends Arceus"
	# Self-contained: the work folder (extracted/, rewritten/, patches/, build output) goes inside the bundle and the
	# data folder comes from the app (~/Library/Application Support/MacNativeGames/PLA). APFS clones keep it cheap.
	if [ "$${PLA_DEV:-0}" != "1" ]; then \
	  rm -rf "$(APP)/Contents/Resources/Work"; mkdir -p "$(APP)/Contents/Resources/Work"; \
	  for d in extracted rewritten patches build; do \
	    if [ -d "$(WORK)/$$d" ]; then cp -cR "$(WORK)/$$d" "$(APP)/Contents/Resources/Work/" 2>/dev/null || cp -R "$(WORK)/$$d" "$(APP)/Contents/Resources/Work/"; fi; done; \
	  rm -rf "$(APP)/Contents/Resources/Work/analysis" "$(APP)/Contents/Resources/Work/venv" \
	         "$(APP)/Contents/Resources/Work/src" "$(APP)/Contents/Resources/Work/logs" \
	         "$(APP)/Contents/Resources/Work/audio-dump.pcm" "$(APP)/Contents/Resources/Work/user-data" \
	         "$(APP)/Contents/Resources/Work/build/obj" "$(APP)/Contents/Resources/Work/build/logs"; fi   # ship the game, not the developer's analysis, sources, logs, saves*
	[ -f "$(OUT)/AppIcon.icns" ] && cp "$(OUT)/AppIcon.icns" "$(APP)/Contents/Resources/" || true
	printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0"><dict>' \
	  '<key>CFBundleExecutable</key><string>Pokemon Legends Arceus</string>' \
	  '<key>CFBundleIdentifier</key><string>org.macnativegames.pla</string>' \
	  '<key>CFBundleName</key><string>Pokemon Legends Arceus</string>' \
	  '<key>CFBundlePackageType</key><string>APPL</string>' \
	  '<key>CFBundleShortVersionString</key><string>1.1.1</string>' \
	  '<key>CFBundleVersion</key><string>1</string>' \
	  '<key>CFBundleIconFile</key><string>AppIcon</string>' \
	  '<key>LSMinimumSystemVersion</key><string>26.0</string>' \
	  '<key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>' \
	  '<key>NSHighResolutionCapable</key><true/>' \
	  '<key>PLAWorkFolder</key><string>$(if $(PLA_DEV),$(abspath $(WORK)),Work)</string>' \
	  '</dict></plist>' > "$(APP)/Contents/Info.plist"
	# Two passes: the seal of a 12 GB Resources tree can still be open when the first pass returns (Spotlight/Finder
	# touch the new bundle), and codesign -v then reports "signature indicates resources must be present".
	codesign --force $(if $(PLA_DEV),,--deep) --sign - "$(APP)"   # a portable bundle seals the game data too, so it needs --deep
	codesign --force $(if $(PLA_DEV),,--deep) --sign - "$(APP)"
	@echo "built $(APP) (self-contained: game data inside Contents/Resources/Work; PLA_DEV=1 builds the development bundle that reads $(abspath $(WORK)))"

clean:
	rm -rf "$(OUT)/obj" "$(OUT)/pla-native"

.PHONY: check-paths
check-paths:
	"$(PY)" scripts/pla_artifact_paths.py "$(WORK)" "$(APP)"
	@"$(PY)" -c 'from pathlib import Path; w,a=map(lambda s:Path(s).resolve(),__import__("sys").argv[1:]); assert a!=w and w not in a.parents and a not in w.parents, "WORK and APP must be separate directories"' "$(WORK)" "$(APP)"

.PHONY: check-new-app
check-new-app: check-paths
	@test ! -e "$(APP)" -a ! -L "$(APP)" || (echo "APP already exists; choose a new output path" >&2; exit 1)

.PHONY: graphics
graphics: $(GFX_OBJECTS)

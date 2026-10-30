# Xenoblade Chronicles 3 native runtime. Self-contained: the Horizon host (runtime/), the rewriter (third_party/d2r), the
# NVN-on-Metal layer (third_party/d2r_gfx), the shader translator (prebuilt/) and the scripts are this folder's own copies,
# forked from games/ot0 (from games/by3, from games/totk; see third_party/COPIES.md); nothing here reads another game's folder. Everything generated
# goes to the work folder.
# Inputs come from scripts/extract_code.py, scripts/merge_romfs.py and the rewrite (see README.md), then:
#   make prepare   # scripts/prepare_runtime.py: tables, RomFS index, fonts config
#   make runtime   # XC3_build/build/xc3-native-<version>
#   make boot      # run it from the work folder (cwd: extracted/ and rewritten/ are read relative to it)
# VERSION picks the game version (v100 or v221); by default the newest one extracted in the work folder.

WORK ?= XC3_build
OUT := $(WORK)/build
VERSIONS := v100 v221
VERSION ?= $(or $(lastword $(sort $(filter $(VERSIONS),$(notdir $(wildcard $(WORK)/extracted/v*))))),v100)
ifeq ($(filter $(VERSION),$(VERSIONS)),)
$(error VERSION=$(VERSION) is not one of $(VERSIONS))
endif
VOUT := $(OUT)/$(VERSION)
DISPLAY_VERSION := $(shell echo $(VERSION) | sed -E 's/^v([0-9])([0-9])([0-9])$$/\1.\2.\3/')
VERSION_DEFINE := -DXC3_GAME_$(shell echo $(VERSION) | tr a-z A-Z)
PY ?= python3
NATIVE_OPT ?= -O2

GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(patsubst %,$(OUT)/obj/%.o,$(GFX_SRC))
GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h)
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn/include

RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
RUNTIME_HEADERS := $(wildcard runtime/*.h)
GENERATED := $(VOUT)/runtime_tables.h $(VOUT)/romfs_config.h $(VOUT)/shared_fonts_config.h $(VOUT)/control_config.h

.PHONY: prepare gfx runtime boot clean
prepare:
	"$(PY)" scripts/prepare_runtime.py --work "$(WORK)" --version $(VERSION)

$(GENERATED):
	@echo "missing $@: run make prepare" && false

$(OUT)/obj/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

$(OUT)/xc3-native-$(VERSION): $(RUNTIME_SRC) $(RUNTIME_HEADERS) $(GENERATED) $(GFX_OBJECTS) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations $(VERSION_DEFINE) \
	  -Ithird_party/d2r_gfx/gfx/nvn/include -I$(VOUT) -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include \
	  $(RUNTIME_SRC) $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a \
	  -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics \
	  -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation \
	  -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@"

gfx: $(GFX_OBJECTS)

runtime: $(OUT)/xc3-native-$(VERSION)

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

# XC3_LOG: where the boot's stderr goes (the host is loud on purpose; every abort names what is missing).
# TOTK_HEADLESS is passed through only when set: the runtime goes headless when it exists at all, even empty.
# TOTK_CONTROLS: the settings panel (key 1) keeps its choices next to it, in the work folder's user-data.
# TOTK_UNCAPPED defaults to 0, as in the app: the game keeps its own pacing (30 fps) on every display.
# TOTK_GPU_ALLOW_SUBMIT defaults to 1, as in the app: the shared layer submits nothing to the GPU without it (TOTK_GPU_ALLOW_SUBMIT=0 for a CPU-only run).
XC3_LOG ?= $(WORK)/logs/boot.log
boot: runtime
	@mkdir -p "$(dir $(XC3_LOG))"
	cd "$(WORK)" && TOTK_GPU_ALLOW_SUBMIT=$${TOTK_GPU_ALLOW_SUBMIT-1} TOTK_UNCAPPED=$${TOTK_UNCAPPED-0} TOTK_CONTROLS="$${TOTK_CONTROLS-$(abspath $(WORK))/user-data/controls.txt}" ./build/xc3-native-$(VERSION) rewritten/$(VERSION) boot 2> "$(abspath $(XC3_LOG))"; echo "exit $$?; log: $(XC3_LOG)"

# The player's mods, made into checked tables: exefs patches (mods/<display version>/<name>/exefs/*.pchtxt) -> <work>/patches/<version>,
# RomFS mods (files under their RomFS paths in $(DATA)/mods/<version>, the folder the app reads) -> its manifest.tsv
# (sizes and SHA-256 of the replacements and the originals).
DATA ?= $(HOME)/Library/Application Support/MacNativeGames/XC3
.PHONY: patches mods app
# The app's startup shaders (scripts/prepare_shader_seed.py): the variants met in play on this Mac, from the work folder's
# translation cache and the app's own (SEED_CACHE, SEED_APP_CACHE), so a fresh app does not translate them while playing. Game-derived:
# it stays in the work folder and the local app.
SEED_CACHE ?= $(WORK)/build/shader-cache/rt-totk-native-v4-unverified
SEED_APP_CACHE ?= $(DATA)/shader-cache
.PHONY: shader-seed
shader-seed:
	"$(PY)" scripts/prepare_shader_seed.py --out "$(OUT)/shader-seed" "$(SEED_CACHE)" "$(SEED_APP_CACHE)"
patches:
	"$(PY)" scripts/prepare_patches.py --work "$(WORK)" --version $(VERSION)
mods:
	"$(PY)" scripts/make_mod_manifest.py "$(DATA)/mods/$(VERSION)" --romfs "$(WORK)/extracted/$(VERSION)/romfs"

# The app: a launcher (settings window with presets, then the game) and, in Contents/Resources/game, everything the game
# reads: the runtime, the shader tools, the rewritten modules and the RomFS, cloned from the work folder (APFS
# copy-on-write, so no extra space on the same volume; a plain copy elsewhere). The game writes only to $(DATA) and
# ~/Library/Logs/MacNativeGames, so the app keeps working without the work folder. Its icon comes from the game's own
# control data (GAME=<base or update NSP> KEYS=<prod.keys>), or the app goes without one.
APP ?= Xenoblade Chronicles 3.app
IN_APP = $(APP)/Contents/Resources/game
GAME ?=
KEYS ?=
$(OUT)/launcher: app/launcher.m Makefile
	xcrun clang -std=gnu11 -O2 -Wall -Wextra -Werror -fobjc-arc app/launcher.m -framework Cocoa -o "$@"
$(OUT)/AppIcon.icns:
	@if [ -n "$(GAME)" ] && [ -n "$(KEYS)" ]; then cd scripts && "$(PY)" extract_app_icon.py "$(GAME)" --keys "$(KEYS)" --title 010074F013262000 --output "$(abspath $@)"; \
	 else echo "no GAME= / KEYS=: the app gets no icon"; fi
app: runtime shader-tools $(OUT)/launcher $(OUT)/AppIcon.icns
	rm -rf "$(APP)" && mkdir -p "$(APP)/Contents/MacOS" "$(IN_APP)/build/shader-service" "$(IN_APP)/build/spirv-cross" \
	  "$(IN_APP)/rewritten" "$(IN_APP)/extracted/$(VERSION)"
	cp "$(OUT)/launcher" "$(APP)/Contents/MacOS/Xenoblade Chronicles 3"
	cp -c "$(OUT)/xc3-native-$(VERSION)" "$(IN_APP)/build/"
	cp -c "$(OUT)/shader-service/totk_xlate" "$(IN_APP)/build/shader-service/"
	cp -c "$(OUT)/spirv-cross/spirv-cross" "$(IN_APP)/build/spirv-cross/"
	cp -cR "$(WORK)/rewritten/$(VERSION)" "$(IN_APP)/rewritten/"
	cp -cR "$(WORK)/extracted/$(VERSION)/romfs" "$(IN_APP)/extracted/$(VERSION)/"
	[ ! -d "$(WORK)/extracted/firmware/shared-fonts" ] || cp -cR "$(WORK)/extracted/firmware" "$(IN_APP)/extracted/"
	[ "$(VERSION)" != v221 ] || [ ! -d "$(WORK)/extracted/aoc" ] || cp -cR "$(WORK)/extracted/aoc" "$(IN_APP)/extracted/"   # add-ons (2.2.1 only)
	[ ! -f "$(OUT)/shader-seed/seed-manifest.json" ] || cp -cR "$(OUT)/shader-seed" "$(IN_APP)/build/"   # make shader-seed first
	[ -f "$(OUT)/AppIcon.icns" ] && cp "$(OUT)/AppIcon.icns" "$(APP)/Contents/Resources/" || true
	printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0"><dict>' \
	  '<key>CFBundleExecutable</key><string>Xenoblade Chronicles 3</string>' \
	  '<key>CFBundleIdentifier</key><string>org.macnativegames.xc3</string>' \
	  '<key>CFBundleName</key><string>Xenoblade Chronicles 3</string>' \
	  '<key>CFBundlePackageType</key><string>APPL</string>' \
	  '<key>CFBundleShortVersionString</key><string>$(DISPLAY_VERSION)</string>' \
	  '<key>CFBundleVersion</key><string>1</string>' \
	  '<key>CFBundleIconFile</key><string>AppIcon</string>' \
	  '<key>LSMinimumSystemVersion</key><string>26.0</string>' \
	  '<key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>' \
	  '<key>NSHighResolutionCapable</key><true/>' \
	  '<key>XC3Version</key><string>$(VERSION)</string>' \
	  '</dict></plist>' > "$(APP)/Contents/Info.plist"
	codesign --force --sign - "$(APP)"
	@echo "built $(APP) for $(DISPLAY_VERSION)"

# Install the app where Launchpad and Spotlight look: /Applications, or ~/Applications when that is not writable.
INSTALL_DIR ?= $(shell [ -w /Applications ] && echo /Applications || echo $(HOME)/Applications)
.PHONY: install
install: app
	mkdir -p "$(INSTALL_DIR)" && rm -rf "$(INSTALL_DIR)/$(APP)" && cp -cR "$(APP)" "$(INSTALL_DIR)/"
	/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$(INSTALL_DIR)/$(APP)"
	@echo "installed $(INSTALL_DIR)/$(APP)"

clean:
	rm -rf "$(OUT)/obj" $(addprefix "$(OUT)/xc3-native-,$(addsuffix ",$(VERSIONS)))

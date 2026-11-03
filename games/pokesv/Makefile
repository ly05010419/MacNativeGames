# Pokémon Violet native runtime. Self-contained: the Horizon host (runtime/), the rewriter (third_party/d2r), the
# NVN-on-Metal layer (third_party/d2r_gfx), the shader translator (prebuilt/) and the scripts are this folder's own copies,
# forked from games/xc3 (third_party/COPIES.md says where each came from). Nothing here reads another game's folder.
# Everything generated goes to the work folder. From a fresh checkout (README.md has the inputs):
#   make doctor    # what is missing, and the target that makes it
#   make venv      # PSV_build/venv: Python 3.14 with requirements-dev.txt
#   make extract BASE=<base.nsp> UPDATE=<update.nsp> KEYS=<prod.keys> FIRMWARE=<firmware NCA folder>
#   make rewrite verify prepare
#   make -j runtime shader-tools
#   make boot      # run it from the work folder (cwd: extracted/ and rewritten/ are read relative to it)
# VERSION picks the game version (v400); by default the newest one extracted in the work folder.

WORK ?= PSV_build
OUT := $(WORK)/build
VERSIONS := v400
VERSION ?= $(or $(lastword $(sort $(filter $(VERSIONS),$(notdir $(wildcard $(WORK)/extracted/v*))))),$(lastword $(VERSIONS)))
ifeq ($(filter $(VERSION),$(VERSIONS)),)
$(error VERSION=$(VERSION) is not one of $(VERSIONS))
endif
VOUT := $(OUT)/$(VERSION)
DISPLAY_VERSION := $(shell echo $(VERSION) | sed -E 's/^v([0-9])([0-9])([0-9])$$/\1.\2.\3/')
VERSION_DEFINE := -DPSV_GAME_$(shell echo $(VERSION) | tr a-z A-Z)
# The work folder's venv when it exists (make venv), else Python 3.14 from the PATH. Absolute, because some steps cd.
VENV := $(WORK)/venv
PY ?= $(if $(wildcard $(VENV)/bin/python),$(abspath $(VENV)/bin/python),python3.14)
NATIVE_OPT ?= -O2
# 26.0, the repository's minimum (Metal 4). The Homebrew opus and libvpx are built for the macOS they were installed on,
# so on macOS 26 they are 26's; only 27 has been tested (after PR #1655).
# The app's Info.plist takes the same value.
MACOS_MIN ?= 26.0
# Homebrew's prefix, asked once (not every Mac has it in /opt/homebrew). structck.py needs llvm@23's llvm-objdump.
BREW := $(or $(shell brew --prefix 2>/dev/null),/opt/homebrew)
OPUS ?= $(BREW)/opt/opus
LIBVPX ?= $(BREW)/opt/libvpx
LLVM_BIN ?= $(BREW)/opt/llvm@23/bin

GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(patsubst %,$(OUT)/obj/%.o,$(GFX_SRC))
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn/include

# Runtime objects are per version: they take VERSION_DEFINE and the generated headers in $(VOUT).
RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
RUNTIME_OBJECTS := $(patsubst %,$(VOUT)/obj/%.o,$(RUNTIME_SRC))
RUNTIME_FLAGS := -std=c11 -Werror -Wno-deprecated-declarations -fobjc-arc $(VERSION_DEFINE) \
  -Ithird_party/d2r_gfx/gfx/nvn/include -I$(VOUT) -I$(OPUS)/include/opus -I$(LIBVPX)/include
GENERATED := $(VOUT)/runtime_tables.h $(VOUT)/romfs_config.h $(VOUT)/shared_fonts_config.h $(VOUT)/control_config.h
# -MMD -MP: each object lists the headers it includes (the .d files below), so an edit rebuilds only what uses it.
CC_COMMON = xcrun clang $(NATIVE_OPT) -g -Wall -Wextra -mmacosx-version-min=$(MACOS_MIN) -MMD -MP

.PHONY: prepare gfx runtime boot clean
prepare:
	"$(PY)" scripts/prepare_runtime.py --work "$(WORK)" --version $(VERSION)

$(GENERATED):
	@echo "missing $@: run make prepare" && false

$(OUT)/obj/%.o: % Makefile
	@mkdir -p "$(dir $@)"
	$(CC_COMMON) $(GFX_FLAGS) -c "$<" -o "$@"

$(VOUT)/obj/%.o: % Makefile | $(GENERATED)
	@mkdir -p "$(dir $@)"
	$(CC_COMMON) $(RUNTIME_FLAGS) -c "$<" -o "$@"

-include $(GFX_OBJECTS:.o=.d) $(RUNTIME_OBJECTS:.o=.d)

$(OUT)/psv-native-$(VERSION): $(RUNTIME_OBJECTS) $(GFX_OBJECTS) Makefile
	xcrun clang -g -mmacosx-version-min=$(MACOS_MIN) $(RUNTIME_OBJECTS) $(GFX_OBJECTS) $(OPUS)/lib/libopus.a $(LIBVPX)/lib/libvpx.a \
	  -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics \
	  -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation \
	  -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@"
	dsymutil "$@" -o "$@.dSYM"

gfx: $(GFX_OBJECTS)

runtime: $(OUT)/psv-native-$(VERSION)

# The runtime shader translation the graphics layer starts on first use: <work>/build/shader-service/psv_xlate (the
# repository's prebuilt NativeAOT translator, checked against its SHA-256) and <work>/build/spirv-cross/spirv-cross,
# built like build_app.py does (same pinned commit, sources and flags as SPIRV-Cross's CMake Release build).
SPIRV_CROSS_COMMIT := aa217aeb6c9f0ace7a0ab233b28807edf45eb165
SPIRV_CROSS_SOURCES := main.cpp spirv_cfg.cpp spirv_cpp.cpp spirv_cross.cpp spirv_cross_parsed_ir.cpp spirv_cross_util.cpp \
  spirv_glsl.cpp spirv_hlsl.cpp spirv_msl.cpp spirv_parser.cpp spirv_reflect.cpp
.PHONY: shader-tools
shader-tools: $(OUT)/shader-service/psv_xlate $(OUT)/spirv-cross/spirv-cross
$(OUT)/shader-service/psv_xlate: prebuilt/macos-arm64/psv_xlate prebuilt/macos-arm64/psv_xlate.sha256
	@mkdir -p "$(dir $@)"
	test "$$(shasum -a 256 "$<" | cut -d' ' -f1)" = "$$(cut -d' ' -f1 "prebuilt/macos-arm64/psv_xlate.sha256")"
	cp "$<" "$@" && chmod +x "$@"
$(OUT)/spirv-cross/spirv-cross:
	@mkdir -p "$(WORK)/src" "$(dir $@)obj"
	test -d "$(WORK)/src/SPIRV-Cross" || git clone -q https://github.com/KhronosGroup/SPIRV-Cross.git "$(WORK)/src/SPIRV-Cross"
	git -C "$(WORK)/src/SPIRV-Cross" checkout -q $(SPIRV_CROSS_COMMIT)
	for f in $(SPIRV_CROSS_SOURCES); do xcrun clang++ -O3 -DNDEBUG -std=c++11 -arch arm64 -Wno-deprecated-declarations \
	  -c "$(WORK)/src/SPIRV-Cross/$$f" -o "$(dir $@)obj/$$f.o" & done; wait
	xcrun clang++ -arch arm64 $(addprefix $(dir $@)obj/,$(addsuffix .o,$(SPIRV_CROSS_SOURCES))) -o "$@"

# PSV_LOG: where the boot's stderr goes (the host is loud on purpose; every abort names what is missing).
# A copy stays as play-<date>.log: test runs write boot.log too, and a player's session must survive them.
# PSV_HEADLESS is passed through only when set: the runtime goes headless when it exists at all, even empty.
# PSV_UNCAPPED defaults to 0, as in the app: the game keeps fixed pacing (the settings panel's frame lock) on every display.
# PSV_ROMFS_MODS defaults to the app's mods folder, $(DATA)/mods/<version> (`make mods` writes its manifest; none = no mods).
# PSV_GPU_ALLOW_SUBMIT defaults to 1, as in the app: the shared layer submits nothing to the GPU without it (PSV_GPU_ALLOW_SUBMIT=0 for a CPU-only run).
PSV_LOG ?= $(WORK)/logs/boot.log
boot: runtime shader-tools
	@mkdir -p "$(dir $(PSV_LOG))"
	cd "$(WORK)" && PSV_GPU_ALLOW_SUBMIT=$${PSV_GPU_ALLOW_SUBMIT-1} PSV_UNCAPPED=$${PSV_UNCAPPED-0} PSV_ROMFS_MODS="$${PSV_ROMFS_MODS-$(DATA)/mods/$(VERSION)}" ./build/psv-native-$(VERSION) rewritten/$(VERSION) boot 2> "$(abspath $(PSV_LOG))"; \
	  e=$$?; k="$(abspath $(dir $(PSV_LOG)))/play-$$(date +%Y%m%d-%H%M).log"; cp "$(abspath $(PSV_LOG))" "$$k"; echo "exit $$e; log: $(PSV_LOG), kept as $$k"

# The player's mods, made into checked tables: exefs patches (mods/<display version>/<name>/exefs/*.pchtxt) -> <work>/patches/<version>,
# RomFS mods (files under their RomFS paths in $(DATA)/mods/<version>, the folder the app reads) -> its manifest.tsv
# (sizes and SHA-256 of the replacements and the originals).
DATA ?= $(HOME)/Library/Application Support/MacNativeGames/PSV
.PHONY: patches mods app
patches:
	"$(PY)" scripts/prepare_patches.py --work "$(WORK)" --version $(VERSION)
mods:
	"$(PY)" scripts/make_mod_manifest.py "$(DATA)/mods/$(VERSION)" --romfs "$(WORK)/extracted/$(VERSION)/romfs"

# The app: a launcher (settings window with presets, then the game) and, in Contents/Resources/game, everything the game
# reads: the runtime, the shader tools, the rewritten modules and the RomFS, cloned from the work folder (APFS
# copy-on-write, so no extra space on the same volume; a plain copy elsewhere). The game writes only to $(DATA) and
# ~/Library/Logs/MacNativeGames, so the app keeps working without the work folder. Its icon comes from the game's own
# control data (GAME=<base or update NSP> KEYS=<prod.keys>), or the app goes without one.
APP ?= Pokemon Violet.app
IN_APP = $(APP)/Contents/Resources/game
GAME ?=
KEYS ?=
$(OUT)/launcher: app/launcher.m Makefile
	xcrun clang -std=gnu11 -O2 -Wall -Wextra -Werror -mmacosx-version-min=$(MACOS_MIN) -fobjc-arc app/launcher.m -framework Cocoa -o "$@"
$(OUT)/AppIcon.icns:
	@if [ -n "$(GAME)" ] && [ -n "$(KEYS)" ]; then cd scripts && "$(PY)" extract_app_icon.py "$(GAME)" --keys "$(KEYS)" --title 01008F6008C5E000 --output "$(abspath $@)"; \
	 else echo "no GAME= / KEYS=: the app gets no icon"; fi
app: runtime shader-tools $(OUT)/launcher $(OUT)/AppIcon.icns
	rm -rf "$(APP)" && mkdir -p "$(APP)/Contents/MacOS" "$(IN_APP)/build/shader-service" "$(IN_APP)/build/spirv-cross" \
	  "$(IN_APP)/rewritten" "$(IN_APP)/extracted/$(VERSION)"
	cp "$(OUT)/launcher" "$(APP)/Contents/MacOS/Pokemon Violet"
	cp -c "$(OUT)/psv-native-$(VERSION)" "$(IN_APP)/build/"
	cp -c "$(OUT)/shader-service/psv_xlate" "$(IN_APP)/build/shader-service/"
	cp -c "$(OUT)/spirv-cross/spirv-cross" "$(IN_APP)/build/spirv-cross/"
	cp -cR "$(WORK)/rewritten/$(VERSION)" "$(IN_APP)/rewritten/"
	cp -cR "$(WORK)/extracted/$(VERSION)/romfs" "$(IN_APP)/extracted/$(VERSION)/"
	[ ! -d "$(WORK)/extracted/firmware/shared-fonts" ] || cp -cR "$(WORK)/extracted/firmware" "$(IN_APP)/extracted/"
	[ -f "$(OUT)/AppIcon.icns" ] && cp "$(OUT)/AppIcon.icns" "$(APP)/Contents/Resources/" || true
	printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0"><dict>' \
	  '<key>CFBundleExecutable</key><string>Pokemon Violet</string>' \
	  '<key>CFBundleIdentifier</key><string>org.macnativegames.pokesv</string>' \
	  '<key>CFBundleName</key><string>Pokemon Violet</string>' \
	  '<key>CFBundlePackageType</key><string>APPL</string>' \
	  '<key>CFBundleShortVersionString</key><string>$(DISPLAY_VERSION)</string>' \
	  '<key>CFBundleVersion</key><string>1</string>' \
	  '<key>CFBundleIconFile</key><string>AppIcon</string>' \
	  '<key>LSMinimumSystemVersion</key><string>$(MACOS_MIN)</string>' \
	  '<key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>' \
	  '<key>NSHighResolutionCapable</key><true/>' \
	  '<key>PSVVersion</key><string>$(VERSION)</string>' \
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
	rm -rf "$(OUT)/obj" $(foreach v,$(VERSIONS),"$(OUT)/$(v)/obj" "$(OUT)/psv-native-$(v)" "$(OUT)/psv-native-$(v).dSYM")

# The steps before make prepare. extract reads the player's own files: BASE (1.0.0) and UPDATE (4.0.0) NSPs, KEYS
# (prod.keys) and FIRMWARE (the firmware's NCA folder, for the shared fonts). The scripts tell the version by content.
BASE ?=
UPDATE ?=
FIRMWARE ?=
MODULES := sdk subsdk0 main
EXEFS := $(WORK)/extracted/$(VERSION)/exefs
REWRITTEN := $(WORK)/rewritten/$(VERSION)
REWRITER := $(wildcard third_party/d2r/patch_nso.py third_party/d2r/call_window.py third_party/d2r/relr.py)
.PHONY: venv extract rewrite verify test doctor
venv:
	python3.14 -m venv "$(VENV)" && "$(VENV)/bin/python" -m pip install -q -r requirements-dev.txt
extract:
	@[ -n "$(BASE)" ] && [ -n "$(UPDATE)" ] && [ -n "$(KEYS)" ] && [ -n "$(FIRMWARE)" ] || \
	  { echo "usage: make extract BASE=<base.nsp> UPDATE=<update.nsp> KEYS=<prod.keys> FIRMWARE=<firmware NCA folder>"; false; }
	"$(PY)" scripts/extract_code.py "$(UPDATE)" --keys "$(KEYS)" --work "$(WORK)"
	"$(PY)" scripts/merge_romfs.py "$(BASE)" "$(UPDATE)" --keys "$(KEYS)" --work "$(WORK)"
	cd scripts && "$(PY)" extract_shared_fonts.py --keys "$(abspath $(KEYS))" --firmware "$(abspath $(FIRMWARE))" \
	  --output "$(abspath $(WORK))/extracted/firmware/shared-fonts"

# The rewrite runs again when the module or the rewriter changes.
$(REWRITTEN)/%.patched.bin: $(EXEFS)/% $(REWRITER)
	DM_RULESET=native DM_PRIVATE_GATES=0 "$(PY)" third_party/d2r/patch_nso.py "$<" "$(REWRITTEN)" $*
rewrite: $(foreach m,$(MODULES),$(REWRITTEN)/$(m).patched.bin)

# structck.py checks every rewritten word against an independent disassembly, equiv.py runs each site against the
# original (unicorn). Both must report 0 failures. Logs: $(WORK)/logs/{structck,equiv}-<module>.log.
verify: rewrite
	@mkdir -p "$(WORK)/logs"
	@for m in $(MODULES); do \
	  LLVM_BIN="$(LLVM_BIN)" DM_VCACHE="$(abspath $(OUT))/cache/verify" "$(PY)" third_party/d2r/structck.py "$(REWRITTEN)" $$m "$(EXEFS)/$$m" \
	    > "$(WORK)/logs/structck-$$m.log" 2>&1; s=$$?; tail -1 "$(WORK)/logs/structck-$$m.log"; [ $$s = 0 ] || exit 1; \
	  "$(PY)" third_party/d2r/equiv.py "$(REWRITTEN)" $$m "$(EXEFS)/$$m" --min-eff 8 > "$(WORK)/logs/equiv-$$m.log" 2>&1; s=$$?; \
	    tail -1 "$(WORK)/logs/equiv-$$m.log"; [ $$s = 0 ] || exit 1; \
	done

test:
	"$(PY)" -m unittest discover -s tests

doctor:
	@PY="$(PY)" WORK="$(WORK)" OUT="$(OUT)" VERSION="$(VERSION)" MACOS_MIN="$(MACOS_MIN)" OPUS="$(OPUS)" LIBVPX="$(LIBVPX)" \
	  LLVM_BIN="$(LLVM_BIN)" GENERATED="$(GENERATED)" sh scripts/doctor.sh

# Luigi's Mansion 3 1.2.0 (0100DCA0064A6000). Self-contained, like games/by3: the Horizon host (runtime/), the rewriter
# (third_party/d2r), the NVN-on-Metal layer (third_party/d2r_gfx), the shader translator (prebuilt/, source in tools/) and
# the scripts are this folder's own copies, forked from games/totk; nothing here reads games/totk.
GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(addprefix build/gfx/,$(addsuffix .o,$(GFX_SRC)))
GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h)
GFX_FLAGS := -DGAME_LM3=1 -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn/include
NATIVE_OPT ?= -O2
PY := .venv/bin/python
# This checkout's own paths go in local.mk (git-ignored), for example:
#   ROM = ~/Games/Luigi's Mansion 3.xci   UPDATE = ~/Games/LM3 1.2.0.nsp   KEYS = ~/keys/prod.keys
-include local.mk
ROM ?=
UPDATE ?=
KEYS ?=
WORK ?= $(abspath work)
HACTOOL := tooling/hactool/hactool
REWRITE := DM_RULESET=native DM_PRIVATE_GATES=0 "$(PY)" third_party/d2r/patch_nso.py
MODULES := main subsdk0 subsdk1 sdk

.PHONY: extract rewrite verify-rewrites runtime boot
extract: $(HACTOOL)
	@test -n "$(ROM)" -a -n "$(UPDATE)" -a -n "$(KEYS)" || { echo "set ROM=<base XCI> UPDATE=<1.2.0 NSP> KEYS=<prod.keys> (or put them in local.mk)"; false; }
	"$(PY)" scripts/extract_exefs.py "$(UPDATE)" --update --keys "$(KEYS)" --output extracted/v120/exefs
	"$(PY)" scripts/prepare_modules.py extracted/v120/exefs --output extracted/v120/elf
	"$(PY)" scripts/extract_update_romfs.py "$(ROM)" "$(UPDATE)" --keys "$(KEYS)" --hactool "$(HACTOOL)" --output extracted/v120/romfs
	"$(PY)" scripts/extract_shared_fonts.py --keys "$(KEYS)" --xci "$(ROM)" --output extracted/firmware/shared-fonts --inventory analysis/firmware-inventory.json

rewrite:
	@mkdir -p rewritten/v120
	for m in $(MODULES); do $(REWRITE) extracted/v120/exefs/$$m rewritten/v120 $$m || exit 1; done

verify-rewrites:
	@mkdir -p analysis
	for m in $(MODULES); do "$(PY)" third_party/d2r/equiv.py rewritten/v120 $$m extracted/v120/exefs/$$m --min-eff 2 --max-extra 16 --mutate 150 --tsv analysis/$$m-equiv.tsv > analysis/$$m-equiv.log || exit 1; tail -1 analysis/$$m-equiv.log; done

build/runtime_tables.h: scripts/prepare_runtime.py
	"$(PY)" scripts/prepare_runtime.py

build/romfs_config.h: extracted/v120/romfs/index.json
	"$(PY)" scripts/prepare_romfs_backend.py

build/shared_fonts_config.h: extracted/firmware/shared-fonts/manifest.json
	"$(PY)" scripts/prepare_shared_fonts.py

RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
build/lm3-native: Makefile $(RUNTIME_SRC) $(wildcard runtime/*.h) build/runtime_tables.h build/romfs_config.h build/shared_fonts_config.h $(GFX_OBJECTS)
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -DGAME_LM3=1 -Ithird_party/d2r_gfx/gfx/nvn/include -Ibuild -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include $(RUNTIME_SRC) $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@"

runtime: build/lm3-native build/shader-service/totk_xlate build/spirv-cross/spirv-cross

# The shader translator service: this folder's prebuilt copy (source and patches in tools/totk_xlate), checked by SHA-256.
build/shader-service/totk_xlate: prebuilt/macos-arm64/totk_xlate
	@mkdir -p "$(dir $@)"
	test "$$(shasum -a 256 "$<" | cut -d" " -f1)" = "$$(cat prebuilt/macos-arm64/totk_xlate.sha256)"
	cp -c "$<" "$@"
# spirv-cross built as build_app.py and games/by3 do (same pinned commit, sources and flags as its CMake Release build).
SPIRV_CROSS_COMMIT := aa217aeb6c9f0ace7a0ab233b28807edf45eb165
SPIRV_CROSS_SOURCES := main.cpp spirv_cfg.cpp spirv_cpp.cpp spirv_cross.cpp spirv_cross_parsed_ir.cpp spirv_cross_util.cpp \
  spirv_glsl.cpp spirv_hlsl.cpp spirv_msl.cpp spirv_parser.cpp spirv_reflect.cpp
build/spirv-cross/spirv-cross:
	@mkdir -p tooling "$(dir $@)obj"
	test -d tooling/spirv-cross || git clone -q https://github.com/KhronosGroup/SPIRV-Cross.git tooling/spirv-cross
	git -C tooling/spirv-cross checkout -q $(SPIRV_CROSS_COMMIT)
	for f in $(SPIRV_CROSS_SOURCES); do xcrun clang++ -O3 -DNDEBUG -std=c++11 -arch arm64 -Wno-deprecated-declarations \
	  -c "tooling/spirv-cross/$$f" -o "$(dir $@)obj/$$f.o" & done; wait
	xcrun clang++ -arch arm64 $(addprefix $(dir $@)obj/,$(addsuffix .o,$(SPIRV_CROSS_SOURCES))) -o "$@"

boot: runtime
	build/lm3-native rewritten/v120 boot

build/gfx/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

# A double-clickable test app next to the game files: it runs this checkout's build/lm3-native in place (no copy of the
# game data), with the save and caches in WORK, and logs the session (a development build: diagnostics on).
TEST_APP ?= LM3 Test.app
.PHONY: test-app
test-app: runtime
	@mkdir -p "$(TEST_APP)/Contents/MacOS" "$(WORK)/logs" "$(WORK)/profile" "$(WORK)/shader-cache"
	@printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0"><dict><key>CFBundleExecutable</key><string>lm3</string><key>CFBundleIdentifier</key><string>local.macnativegames.lm3.test</string>' \
	  '<key>CFBundleName</key><string>LM3 Test</string><key>CFBundlePackageType</key><string>APPL</string><key>LSMinimumSystemVersion</key><string>26.0</string>' \
	  '<key>NSHighResolutionCapable</key><true/></dict></plist>' > "$(TEST_APP)/Contents/Info.plist"
	@printf '%s\n' '#!/bin/zsh' 'cd "$(abspath .)" || exit 1' \
	  'export LM3_CORE_LOG=1 TOTK_SLOT_OWNER=user TOTK_PROFILE_DIR="$(WORK)/profile" TOTK_SHADER_CACHE="$(WORK)/shader-cache" TOTK_ROMFS_PATCHES=0' \
	  'export TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 TOTK_GPU_BUDGET_MB=8000' \
	  'exec build/lm3-native rewritten/v120 boot > "$(WORK)/logs/test-app-$$(date +%Y%m%d-%H%M%S).log" 2>&1' > "$(TEST_APP)/Contents/MacOS/lm3"
	@chmod +x "$(TEST_APP)/Contents/MacOS/lm3"
	@echo "test app: $(TEST_APP)"

# The app: a small bundle that runs the game from this folder (named in its launcher script; nothing large is copied).
# Saves and the shader cache live in ~/Library/Application Support/MacNativeGames/LM3 (the first build copies the test
# build's profile and cache there when that folder does not exist yet), the log in ~/Library/Logs/MacNativeGames/lm3.log.
# Icon: the game's own, from the control data of ROM (or none). Cheats: build/cheats.txt from switch-cheats-db when it
# has this version's build ID (scripts/fetch_cheats.py); without them the app has no Cheats menu.
APP ?= Luigi's Mansion 3.app
APP_DATA := $(HOME)/Library/Application Support/MacNativeGames/LM3
build/AppIcon.icns:
	-"$(PY)" scripts/extract_app_icon.py "$(ROM)" --keys "$(KEYS)" --output "$@" || echo "no icon: the app gets the default one"
.PHONY: app cheats
cheats:
	"$(PY)" scripts/fetch_cheats.py --main extracted/v120/exefs/main --title 0100DCA0064A6000 --output build/cheats.txt
app: runtime build/AppIcon.icns cheats
	rm -rf "$(APP)" && mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources"
	@if [ ! -d "$(APP_DATA)" ]; then mkdir -p "$(APP_DATA)"; \
	  [ -d "$(WORK)/profile" ] && cp -Rc "$(WORK)/profile" "$(APP_DATA)/profile"; \
	  [ -d "$(WORK)/shader-cache" ] && cp -Rc "$(WORK)/shader-cache" "$(APP_DATA)/shader-cache"; true; fi
	[ -f build/AppIcon.icns ] && cp build/AppIcon.icns "$(APP)/Contents/Resources/" || true
	printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0"><dict>' \
	  '<key>CFBundleExecutable</key><string>LM3</string>' \
	  '<key>CFBundleIdentifier</key><string>org.macnativegames.lm3</string>' \
	  "<key>CFBundleName</key><string>Luigi's Mansion 3</string>" \
	  "<key>CFBundleDisplayName</key><string>Luigi's Mansion 3</string>" \
	  '<key>CFBundlePackageType</key><string>APPL</string>' \
	  '<key>CFBundleShortVersionString</key><string>1.2.0</string>' \
	  '<key>CFBundleVersion</key><string>1</string>' \
	  '<key>CFBundleIconFile</key><string>AppIcon</string>' \
	  '<key>LSMinimumSystemVersion</key><string>26.0</string>' \
	  '<key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>' \
	  '<key>NSHighResolutionCapable</key><true/>' \
	  '</dict></plist>' > "$(APP)/Contents/Info.plist"
	printf '%s\n' '#!/bin/zsh' 'cd "$(abspath .)" || exit 1' \
	  'D="$$HOME/Library/Application Support/MacNativeGames/LM3"; L="$$HOME/Library/Logs/MacNativeGames"' \
	  'mkdir -p "$$D/profile" "$$D/shader-cache" "$$L"; [ -f "$$L/lm3.log" ] && mv -f "$$L/lm3.log" "$$L/lm3.previous.log"' \
	  '[ -f build/cheats.txt ] && export TOTK_CHEATS_FILE="$$PWD/build/cheats.txt"' \
	  'export TOTK_SLOT_OWNER=user TOTK_PROFILE_DIR="$$D/profile" TOTK_SHADER_CACHE="$$D/shader-cache" TOTK_ROMFS_PATCHES=0' \
	  'export TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 TOTK_GPU_BUDGET_MB=8000' \
	  'exec build/lm3-native rewritten/v120 boot > "$$L/lm3.log" 2>&1' > "$(APP)/Contents/MacOS/LM3"
	chmod +x "$(APP)/Contents/MacOS/LM3"
	codesign --force --sign - "$(APP)"
	@echo "built $(APP) (it runs the game from $(abspath .))"

# hactool at the commit build_app.py pins, with this folder's patch (tools/hactool.patch: a private title-key file and the
# base Program NCA read in place inside the XCI/NSP, as build_app.py patches it).
HACTOOL_COMMIT := 1d64a83450e025622f3468c28fc4164dad2c5ef6
$(HACTOOL): tools/hactool.patch
	@mkdir -p tooling
	test -d tooling/hactool || git clone -q https://github.com/SciresM/hactool.git tooling/hactool
	git -C tooling/hactool checkout -q -f $(HACTOOL_COMMIT) && git -C tooling/hactool apply "$(abspath tools/hactool.patch)"
	cp tooling/hactool/config.mk.template tooling/hactool/config.mk && $(MAKE) -C tooling/hactool

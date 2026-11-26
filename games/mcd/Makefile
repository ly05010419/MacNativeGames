# Minecraft Dungeons native runtime. Self-contained: the Horizon host (runtime/), the rewriter (third_party/d2r), the NVN-on-Metal
# layer (third_party/d2r_gfx), the shader translator (prebuilt/) and the scripts are this folder's own copies, forked from
# games/totk; nothing here reads games/totk. Everything generated goes to the work folder.
# Inputs come from scripts/extract_code.py, scripts/merge_romfs.py and the rewrite (see README.md), then:
#   make prepare   # scripts/prepare_runtime.py: tables, RomFS index, fonts config
#   make runtime   # MCD_build/build/mcd-native
#   make boot      # run it from the work folder (cwd: extracted/ and rewritten/ are read relative to it)

WORK ?= MCD_build
OUT := $(WORK)/build
PY ?= python3
OPUS_PREFIX ?= $(or $(shell python3 ../../tools/deps prefix opus),$(error tools/deps could not make opus))
VPX_PREFIX ?= $(or $(shell python3 ../../tools/deps prefix libvpx),$(error tools/deps could not make libvpx))
NATIVE_OPT ?= -O2

GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(patsubst %,$(OUT)/obj/%.o,$(GFX_SRC))
GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h)
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn/include

RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
RUNTIME_HEADERS := $(wildcard runtime/*.h runtime/*.inc)
GENERATED := $(OUT)/runtime_tables.h $(OUT)/romfs_config.h $(OUT)/shared_fonts_config.h $(OUT)/aoc_config.h

.PHONY: prepare runtime boot clean
prepare:
	"$(PY)" scripts/prepare_runtime.py --work "$(WORK)"

$(GENERATED):
	@echo "missing $@: run make prepare" && false

$(OUT)/obj/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

$(OUT)/mcd-native: $(RUNTIME_SRC) $(RUNTIME_HEADERS) $(GENERATED) $(GFX_OBJECTS) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations \
	  -Ithird_party/d2r_gfx/gfx/nvn/include -I$(OUT) -I$(OPUS_PREFIX)/include/opus -I$(VPX_PREFIX)/include \
	  $(RUNTIME_SRC) $(GFX_OBJECTS) $(OPUS_PREFIX)/lib/libopus.a $(VPX_PREFIX)/lib/libvpx.a \
	  -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics \
	  -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation \
	  -framework AudioToolbox -framework VideoToolbox -framework CoreMedia -framework GameController -framework CoreHaptics -framework IOKit -o "$@"

runtime: $(OUT)/mcd-native

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

# BY3_LOG: where the boot's stderr goes (the host is loud on purpose; every abort names what is missing).
BY3_LOG ?= $(WORK)/logs/boot.log
boot: runtime
	@mkdir -p "$(dir $(BY3_LOG))"
	cd "$(WORK)" && TOTK_HEADLESS=$${TOTK_HEADLESS-} ./build/mcd-native rewritten/v1769472 boot 2> "$(abspath $(BY3_LOG))"; echo "exit $$?; log: $(BY3_LOG)"

# The player's mods, made into checked tables: exefs patches (mods/1.2.0/<name>/exefs/*.pchtxt) -> <work>/patches,
# RomFS mods (mods/<name>/romfs/...) -> their manifest.tsv (sizes and SHA-256 of the replacements and the originals).
.PHONY: patches mods app
patches:
	"$(PY)" scripts/prepare_patches.py --work "$(WORK)"
mods:
	for d in mods/*/romfs; do [ -d "$$d" ] && "$(PY)" scripts/make_mod_manifest.py "$$d" --romfs "$(WORK)/extracted/v1769472/romfs"; done; true

# The app: a self-contained bundle carrying the game runtime, rewritten modules, shader tools, shared fonts, and the
# signed RomFS data bundle (Contents/Helpers/GameData-v1769472.bundle). The 13 GB RomFS image lives once inside the
# target .app bundle: if $(APP)/Contents/Helpers/GameData-v1769472.bundle is already signed and present, packaging skips
# data import/signing and only updates the compiled binaries and runtime files in $(APP); otherwise it imports and signs
# the data bundle directly in $(APP) and replaces $(WORK)/extracted/v1769472/romfs/romfs.bin with a symlink to it so the
# work folder does not keep duplicate multi-gigabyte copies.
APP ?= Minecraft Dungeons.app
GAME ?=
KEYS ?=
APP_DATA_BUNDLE := $(APP)/Contents/Helpers/GameData-v1769472.bundle
$(OUT)/launcher: app/launcher.m app/user_picker_mac.h runtime/user_store.h runtime/user_avatar.h Makefile
	xcrun clang -std=gnu11 -O2 -Wall -Wextra -Werror -fobjc-arc app/launcher.m -framework Cocoa -o "$@"
$(OUT)/AppIcon.icns:
	@if [ -n "$(GAME)" ] && [ -n "$(KEYS)" ]; then cd scripts && "$(PY)" extract_app_icon.py "$(GAME)" --keys "$(KEYS)" --title 01006c100ec08000 --output "$(abspath $@)"; \
	 else echo "no GAME= / KEYS=: the app gets no icon"; fi
.PHONY: shader-seed
shader-seed: shader-tools
	"$(PY)" scripts/prepare_shader_seed.py --index "$(WORK)/extracted/v1769472/romfs/index.json" \
	  --image "$(WORK)/extracted/v1769472/romfs/romfs.bin" --output "$(OUT)/shader-seed" \
	  --translator "$(OUT)/shader-service/totk_xlate" --spirv-cross "$(OUT)/spirv-cross/spirv-cross" --reuse
	"$(PY)" scripts/compile_shader_libs.py --cache "$(OUT)/shader-seed" --output "$(OUT)/shader-libs"

app: runtime shader-seed $(OUT)/launcher $(OUT)/AppIcon.icns
	mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Helpers" \
	  "$(APP)/Contents/Resources/mcd/build/shader-service" \
	  "$(APP)/Contents/Resources/mcd/build/spirv-cross" \
	  "$(APP)/Contents/Resources/mcd/rewritten/v1769472" \
	  "$(APP)/Contents/Resources/mcd/extracted/v1769472/romfs" \
	  "$(APP)/Contents/Resources/mcd/extracted/firmware"
	@if [ -f "$(APP_DATA_BUNDLE)/Contents/_CodeSignature/CodeResources" ] && [ -f "$(APP_DATA_BUNDLE)/Contents/Resources/romfs.bin" ]; then \
	  echo "reusing existing signed $(APP_DATA_BUNDLE)"; \
	else \
	  echo "building and signing $(APP_DATA_BUNDLE) directly in $(APP)..."; \
	  rm -rf "$(APP_DATA_BUNDLE).partial" && mkdir -p "$(APP_DATA_BUNDLE).partial/Contents/Resources"; \
	  if [ -f "$(OUT)/data-bundles/GameData-v1769472.bundle/Contents/Resources/romfs.bin" ]; then \
	    mv "$(OUT)/data-bundles/GameData-v1769472.bundle/Contents/Resources/romfs.bin" "$(APP_DATA_BUNDLE).partial/Contents/Resources/romfs.bin"; \
	  elif [ -f "$(WORK)/extracted/v1769472/romfs/romfs.bin" ] && [ ! -L "$(WORK)/extracted/v1769472/romfs/romfs.bin" ]; then \
	    mv "$(WORK)/extracted/v1769472/romfs/romfs.bin" "$(APP_DATA_BUNDLE).partial/Contents/Resources/romfs.bin"; \
	  else \
	    cp -c "$(WORK)/extracted/v1769472/romfs/romfs.bin" "$(APP_DATA_BUNDLE).partial/Contents/Resources/romfs.bin" 2>/dev/null || cp "$(WORK)/extracted/v1769472/romfs/romfs.bin" "$(APP_DATA_BUNDLE).partial/Contents/Resources/romfs.bin"; \
	  fi; \
	  printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	    '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	    '<plist version="1.0"><dict>' \
	    '<key>CFBundleIdentifier</key><string>org.macnativegames.mcd.data.v1769472</string>' \
	    '<key>CFBundleName</key><string>Game data</string>' \
	    '<key>CFBundlePackageType</key><string>BNDL</string>' \
	    '<key>CFBundleShortVersionString</key><string>1.17.0.0</string>' \
	    '<key>CFBundleVersion</key><string>1</string>' \
	    '</dict></plist>' > "$(APP_DATA_BUNDLE).partial/Contents/Info.plist"; \
	  codesign --force --sign - "$(APP_DATA_BUNDLE).partial"; \
	  rm -rf "$(APP_DATA_BUNDLE)" && mv "$(APP_DATA_BUNDLE).partial" "$(APP_DATA_BUNDLE)"; \
	fi
	rm -rf "$(OUT)/data-bundles"
	ln -sf "$$(cd "$(APP_DATA_BUNDLE)/Contents/Resources" && pwd)/romfs.bin" "$(WORK)/extracted/v1769472/romfs/romfs.bin"
	cp "$(OUT)/launcher" "$(APP)/Contents/MacOS/Minecraft Dungeons"
	[ -f "$(OUT)/AppIcon.icns" ] && cp "$(OUT)/AppIcon.icns" "$(APP)/Contents/Resources/" || true
	cp -c "$(OUT)/mcd-native" "$(APP)/Contents/Resources/mcd/build/mcd-native" 2>/dev/null || cp "$(OUT)/mcd-native" "$(APP)/Contents/Resources/mcd/build/mcd-native"
	cp -c "$(OUT)/shader-service/totk_xlate" "$(APP)/Contents/Resources/mcd/build/shader-service/totk_xlate" 2>/dev/null || cp "$(OUT)/shader-service/totk_xlate" "$(APP)/Contents/Resources/mcd/build/shader-service/totk_xlate"
	cp -c "$(OUT)/spirv-cross/spirv-cross" "$(APP)/Contents/Resources/mcd/build/spirv-cross/spirv-cross" 2>/dev/null || cp "$(OUT)/spirv-cross/spirv-cross" "$(APP)/Contents/Resources/mcd/build/spirv-cross/spirv-cross"
	rm -rf "$(APP)/Contents/Resources/mcd/build/shader-cache"
	[ -d "$(OUT)/shader-cache" ] && (cp -cR "$(OUT)/shader-cache" "$(APP)/Contents/Resources/mcd/build/shader-cache" 2>/dev/null || cp -R "$(OUT)/shader-cache" "$(APP)/Contents/Resources/mcd/build/shader-cache") || true
	"$(PY)" scripts/package_shader_seed.py --seed "$(OUT)/shader-seed" --libraries "$(OUT)/shader-libs" \
	  --destination "$(APP)/Contents/Resources/mcd/build"
	cp -c "$(WORK)/rewritten/v1769472/main.patched.bin" "$(APP)/Contents/Resources/mcd/rewritten/v1769472/main.patched.bin" 2>/dev/null || cp "$(WORK)/rewritten/v1769472/main.patched.bin" "$(APP)/Contents/Resources/mcd/rewritten/v1769472/main.patched.bin"
	cp -c "$(WORK)/rewritten/v1769472/subsdk0.patched.bin" "$(APP)/Contents/Resources/mcd/rewritten/v1769472/subsdk0.patched.bin" 2>/dev/null || cp "$(WORK)/rewritten/v1769472/subsdk0.patched.bin" "$(APP)/Contents/Resources/mcd/rewritten/v1769472/subsdk0.patched.bin"
	cp -c "$(WORK)/rewritten/v1769472/sdk.patched.bin" "$(APP)/Contents/Resources/mcd/rewritten/v1769472/sdk.patched.bin" 2>/dev/null || cp "$(WORK)/rewritten/v1769472/sdk.patched.bin" "$(APP)/Contents/Resources/mcd/rewritten/v1769472/sdk.patched.bin"
	rm -rf "$(APP)/Contents/Resources/mcd/extracted/v1769472/aoc"
	[ -d "$(WORK)/extracted/v1769472/aoc" ] && (cp -cR "$(WORK)/extracted/v1769472/aoc" "$(APP)/Contents/Resources/mcd/extracted/v1769472/aoc" 2>/dev/null || cp -R "$(WORK)/extracted/v1769472/aoc" "$(APP)/Contents/Resources/mcd/extracted/v1769472/aoc") || true
	rm -rf "$(APP)/Contents/Resources/mcd/extracted/firmware/shared-fonts"
	cp -cR "$(WORK)/extracted/firmware/shared-fonts" "$(APP)/Contents/Resources/mcd/extracted/firmware/shared-fonts" 2>/dev/null || cp -R "$(WORK)/extracted/firmware/shared-fonts" "$(APP)/Contents/Resources/mcd/extracted/firmware/shared-fonts"
	cp -c "$(WORK)/extracted/v1769472/romfs/runtime-index.bin" "$(APP)/Contents/Resources/mcd/extracted/v1769472/romfs/runtime-index.bin" 2>/dev/null || cp "$(WORK)/extracted/v1769472/romfs/runtime-index.bin" "$(APP)/Contents/Resources/mcd/extracted/v1769472/romfs/runtime-index.bin"
	cp -c "$(WORK)/extracted/v1769472/romfs/index.json" "$(APP)/Contents/Resources/mcd/extracted/v1769472/romfs/index.json" 2>/dev/null || cp "$(WORK)/extracted/v1769472/romfs/index.json" "$(APP)/Contents/Resources/mcd/extracted/v1769472/romfs/index.json"
	[ -f "$(WORK)/extracted/v1769472/romfs/.romfs-verified-v1" ] && cp "$(WORK)/extracted/v1769472/romfs/.romfs-verified-v1" "$(APP)/Contents/Resources/mcd/extracted/v1769472/romfs/.romfs-verified-v1" || true
	ln -sf "../../../../../Helpers/GameData-v1769472.bundle/Contents/Resources/romfs.bin" "$(APP)/Contents/Resources/mcd/extracted/v1769472/romfs/romfs.bin"
	printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0"><dict>' \
	  '<key>CFBundleExecutable</key><string>Minecraft Dungeons</string>' \
	  '<key>CFBundleIdentifier</key><string>org.macnativegames.mcd</string>' \
	  '<key>CFBundleName</key><string>Minecraft Dungeons</string>' \
	  '<key>CFBundlePackageType</key><string>APPL</string>' \
	  '<key>CFBundleShortVersionString</key><string>1.17.0.0</string>' \
	  '<key>CFBundleVersion</key><string>1</string>' \
	  '<key>CFBundleIconFile</key><string>AppIcon</string>' \
	  '<key>LSMinimumSystemVersion</key><string>26.0</string>' \
	  '<key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>' \
	  '<key>NSHighResolutionCapable</key><true/>' \
	  '</dict></plist>' > "$(APP)/Contents/Info.plist"
	codesign --force --sign - "$(APP)"
	@echo "built self-contained $(APP)"

# Install the (small) app where Launchpad and Spotlight look: /Applications, or ~/Applications when that is not writable.
INSTALL_DIR ?= $(shell [ -w /Applications ] && echo /Applications || echo $(HOME)/Applications)
.PHONY: install
install: app
	mkdir -p "$(INSTALL_DIR)" && rm -rf "$(INSTALL_DIR)/$(APP)" && cp -R "$(APP)" "$(INSTALL_DIR)/"
	/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$(INSTALL_DIR)/$(APP)"
	@echo "installed $(INSTALL_DIR)/$(APP)"

clean:
	rm -rf "$(OUT)/obj" "$(OUT)/mcd-native"

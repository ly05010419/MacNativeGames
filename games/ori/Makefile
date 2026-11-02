# Ori and the Blind Forest: Definitive Edition 1.0.1 (010061D00DB74000). Self-contained, like games/by3: the Horizon host
# (runtime/), the rewriter (third_party/d2r), the NVN-on-Metal layer (third_party/d2r_gfx), the shader translator
# (prebuilt/, source in tools/) and the scripts are this folder's own copies; nothing here reads another games/ folder.
# scripts/build_ori.py runs every step from the game file; the targets below are the parts it calls.
GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(addprefix build/gfx/,$(addsuffix .o,$(GFX_SRC)))
GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h)
GFX_FLAGS := -DGAME_ORI=1 -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn/include
NATIVE_OPT ?= -O2
PY := .venv/bin/python
V := v101
export GAME_DIR := $(V)
HACTOOL := tooling/hactool/hactool
REWRITE := DM_RULESET=native DM_PRIVATE_GATES=0 "$(PY)" third_party/d2r/patch_nso.py
MODULES := main subsdk0 sdk

.PHONY: rewrite runtime hactool
rewrite:
	@mkdir -p rewritten/$(V)
	for m in $(MODULES); do $(REWRITE) extracted/$(V)/exefs/$$m rewritten/$(V) $$m || exit 1; done

build/runtime_tables.h: scripts/prepare_runtime.py
	"$(PY)" scripts/prepare_runtime.py

build/romfs_config.h: extracted/$(V)/romfs/index.json
	"$(PY)" scripts/prepare_romfs_backend.py

build/shared_fonts_config.h: extracted/firmware/shared-fonts/manifest.json
	"$(PY)" scripts/prepare_shared_fonts.py

RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
build/ori-native: Makefile $(RUNTIME_SRC) $(wildcard runtime/*.h) build/runtime_tables.h build/romfs_config.h build/shared_fonts_config.h $(GFX_OBJECTS)
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -DGAME_ORI=1 -Ithird_party/d2r_gfx/gfx/nvn/include -Ibuild -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include $(RUNTIME_SRC) $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@"

runtime: build/ori-native build/shader-service/totk_xlate build/spirv-cross/spirv-cross

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

build/gfx/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

# hactool at the commit build_app.py pins, with this folder's patch (tools/hactool.patch: a private title-key file and the
# base Program NCA read in place inside the XCI/NSP, as build_app.py patches it).
HACTOOL_COMMIT := 1d64a83450e025622f3468c28fc4164dad2c5ef6
hactool: $(HACTOOL)
$(HACTOOL): tools/hactool.patch
	@mkdir -p tooling
	test -d tooling/hactool || git clone -q https://github.com/SciresM/hactool.git tooling/hactool
	git -C tooling/hactool checkout -q -f $(HACTOOL_COMMIT) && git -C tooling/hactool apply "$(abspath tools/hactool.patch)"
	cp tooling/hactool/config.mk.template tooling/hactool/config.mk && $(MAKE) -C tooling/hactool

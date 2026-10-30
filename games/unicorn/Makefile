# 圣兽之王的独立原生运行时；游戏数据和生成文件保存在工作目录。

WORK ?= $(HOME)/Games/Unicorn-build-1.05
OUT := $(WORK)/build
PY ?= python3
NATIVE_OPT ?= -O2

GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(patsubst %,$(OUT)/obj/%.o,$(GFX_SRC))
GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h)
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn/include

RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
RUNTIME_HEADERS := $(wildcard runtime/*.h)
GENERATED := $(OUT)/runtime_tables.h $(OUT)/romfs_config.h $(OUT)/shared_fonts_config.h $(OUT)/addons_config.h

.PHONY: prepare runtime boot clean
prepare:
	"$(PY)" scripts/prepare_runtime.py --work "$(WORK)"

$(GENERATED):
	@echo "missing $@: run make prepare" && false

$(OUT)/obj/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

$(OUT)/unicorn-native: $(RUNTIME_SRC) $(RUNTIME_HEADERS) $(GENERATED) $(GFX_OBJECTS) Makefile
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations \
	  -Ithird_party/d2r_gfx/gfx/nvn/include -I$(OUT) -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include \
	  $(RUNTIME_SRC) $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a \
	  -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics \
	  -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation \
	  -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@"

runtime: $(OUT)/unicorn-native

# 着色器工具使用本目录的翻译器和固定版本源码，构建不访问网络或其他游戏目录。
SPIRV_CROSS_SOURCES := main.cpp spirv_cfg.cpp spirv_cpp.cpp spirv_cross.cpp spirv_cross_parsed_ir.cpp spirv_cross_util.cpp \
  spirv_glsl.cpp spirv_hlsl.cpp spirv_msl.cpp spirv_parser.cpp spirv_reflect.cpp
SPIRV_CROSS_OBJECTS := $(addprefix $(OUT)/spirv-cross/obj/,$(addsuffix .o,$(SPIRV_CROSS_SOURCES)))
.PHONY: shader-tools
shader-tools: $(OUT)/shader-service/totk_xlate $(OUT)/spirv-cross/spirv-cross
$(OUT)/shader-service/totk_xlate: prebuilt/macos-arm64/totk_xlate prebuilt/macos-arm64/totk_xlate.sha256
	@mkdir -p "$(dir $@)"
	test "$$(shasum -a 256 "$<" | cut -d' ' -f1)" = "$$(cut -d' ' -f1 "prebuilt/macos-arm64/totk_xlate.sha256")"
	cp "$<" "$@" && chmod +x "$@"
$(OUT)/spirv-cross/obj/%.cpp.o: third_party/spirv-cross/%.cpp $(wildcard third_party/spirv-cross/*.h third_party/spirv-cross/*.hpp) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang++ -O3 -DNDEBUG -std=c++11 -arch arm64 -Wno-deprecated-declarations -c "$<" -o "$@"
$(OUT)/spirv-cross/spirv-cross: $(SPIRV_CROSS_OBJECTS)
	xcrun clang++ -arch arm64 $(SPIRV_CROSS_OBJECTS) -o "$@"

UNICORN_LOG ?= $(WORK)/boot.log
boot: runtime shader-tools
	cd "$(WORK)" && TOTK_GPU_ALLOW_SUBMIT=1 ./build/unicorn-native rewritten/v262144 boot > "$(abspath $(UNICORN_LOG))" 2>&1

APP ?= $(WORK)/圣兽之王.app
$(OUT)/launcher: app/launcher.m runtime/game_version.h
	xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -fobjc-arc app/launcher.m -framework Cocoa -o "$@"

.PHONY: app
app: runtime shader-tools $(OUT)/launcher
	mkdir -p "$(APP)/Contents/MacOS"
	cp "$(OUT)/launcher" "$(APP)/Contents/MacOS/Unicorn Overlord"
	cp app/Info.plist "$(APP)/Contents/Info.plist"
	/usr/libexec/PlistBuddy -c "Set :UnicornWorkFolder $(abspath $(WORK))" "$(APP)/Contents/Info.plist"
	codesign --force --sign - "$(APP)"

$(OUT)/test-opus-multistream: tests/opus_multistream.c runtime/opus_multistream.c runtime/runtime.h
	@mkdir -p "$(dir $@)"
	xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -Iruntime -I/opt/homebrew/opt/opus/include/opus \
	  tests/opus_multistream.c runtime/opus_multistream.c /opt/homebrew/opt/opus/lib/libopus.a -o "$@"

.PHONY: test-opus
test-opus: $(OUT)/test-opus-multistream
	"$(OUT)/test-opus-multistream"

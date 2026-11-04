# Self-contained Tomodachi runtime; all build output stays outside the checkout.
WORK ?= /tmp/Tomodachi_build/host
OUT := $(WORK)/build
BACKEND ?= metal4
ifneq ($(BACKEND),metal4)
$(error Only Metal 4 is supported; macOS 26 or newer is required)
endif
BACKEND_FLAGS := -mmacosx-version-min=26.0 -DDM_NVN_M3=0
BINARY := $(OUT)/tomodachi-native-metal4
GFX_OUT := $(OUT)/metal4/gfx
GFX := third_party/d2r_gfx
GFX_SRC := $(wildcard $(GFX)/gfx/nvn/*.c $(GFX)/gfx/nvn/*.m $(GFX)/gfx/nvn/*.S $(GFX)/gfx/tex/*.c $(GFX)/gfx/tex/*.m)
GFX_OBJ := $(patsubst $(GFX)/%,$(GFX_OUT)/%.o,$(GFX_SRC))
TITLE_RUNTIME := $(wildcard runtime/*.c runtime/*.m runtime/*.S)
FLAGS := -O2 -g $(BACKEND_FLAGS) -DDM_NVN_SERIAL_REPLAY=1 -DNATIVE_GAME_TITLE='"Tomodachi Life: Living the Dream"' -DNATIVE_APPLICATION_ID=0x010051F0207B2000ULL -Iruntime -I$(GFX)/gfx/nvn -I$(GFX)/gfx/nvn/include -I$(OUT) -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include
FRAMEWORKS := Metal MetalFX Foundation QuartzCore ImageIO CoreGraphics CoreText UniformTypeIdentifiers Cocoa CoreVideo CoreFoundation AudioToolbox GameController CoreHaptics IOKit
.PHONY: all graphics check-toolchain
all: $(BINARY)
graphics: $(GFX_OBJ)
$(GFX_OUT)/%.o: $(GFX)/% $(wildcard $(GFX)/gfx/nvn/*.h $(GFX)/gfx/nvn/include/*.h) Makefile | check-toolchain
	@mkdir -p "$(dir $@)"
	xcrun clang -std=gnu11 $(FLAGS) -fobjc-arc -Wno-unused-function -c "$<" -o "$@"
$(BINARY): $(TITLE_RUNTIME) $(wildcard runtime/*.h) $(OUT)/runtime_tables.h $(OUT)/romfs_config.h $(OUT)/shared_fonts_config.h $(OUT)/title_config.h $(GFX_OBJ) Makefile | check-toolchain
	xcrun clang -std=c11 $(FLAGS) -Wno-deprecated-declarations -fobjc-arc $(TITLE_RUNTIME) $(GFX_OBJ) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a $(addprefix -framework ,$(FRAMEWORKS)) -o "$@"

check-toolchain:
	@python3 -c 'from pathlib import Path; from tools.common import outside_repository; outside_repository(Path(__import__("sys").argv[1]), Path.cwd().parents[1])' "$(WORK)"
	@python3 tools/check_metal4_sdk.py

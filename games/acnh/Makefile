WORK ?= ACNH_build
OUT := $(WORK)/build
NATIVE_OPT ?= -O2

GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(patsubst %,$(OUT)/obj/%.o,$(GFX_SRC))
GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h)
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -DGAME_ACNH=1 -Ithird_party/d2r_gfx/gfx/nvn/include -I$(OUT) -include $(OUT)/version-config.h
RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
RUNTIME_HEADERS := $(wildcard runtime/*.h)
GENERATED := $(OUT)/runtime_tables.h $(OUT)/romfs_config.h $(OUT)/shared_fonts_config.h $(OUT)/version-config.h $(OUT)/fps_config.h $(OUT)/dlc_config.h

.PHONY: all prepare runtime clean

all: $(OUT)/acnh-native

# build_host.py writes the game version; a host work folder made before 3.0.1 support (always 1.2.1) gets 1.2.1's.
$(OUT)/version-config.h:
	@test -f "$(OUT)/runtime_tables.h" || { echo "$(OUT) has no host tables: run build_host.py first" >&2; exit 1; }
	printf '#define ACNH_VERSION_RAW 458752\n' > "$@"

# Existing work folders and builds without optional speed-correction data keep the original 30 FPS behavior.
$(OUT)/fps_config.h:
	@mkdir -p "$(OUT)"
	printf '#define ACNH_FPS_ENABLED 0\n' > "$@"

$(OUT)/dlc_config.h:
	@mkdir -p "$(OUT)"
	printf '#define ACNH_DLC_COUNT 0\n#define ACNH_DLC_MARKER "MNG_ACNH_DLC:none"\n' > "$@"

$(OUT)/obj/%.o: % $(GFX_HEADERS) $(OUT)/version-config.h
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

$(OUT)/acnh-native: $(RUNTIME_SRC) $(RUNTIME_HEADERS) $(GENERATED) $(GFX_OBJECTS)
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Wno-error -Wno-deprecated-declarations \
	  -Ithird_party/d2r_gfx/gfx/nvn/include -I$(OUT) -include $(OUT)/version-config.h -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include \
	  -I/opt/homebrew/opt/libvorbis/include -I/opt/homebrew/opt/libogg/include \
	  $(RUNTIME_SRC) $(GFX_OBJECTS) /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a \
	  /opt/homebrew/opt/libvorbis/lib/libvorbis.a /opt/homebrew/opt/libogg/lib/libogg.a \
	  -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics \
	  -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation \
	  -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -framework VideoToolbox -framework CoreMedia -o "$@"

clean:
	rm -rf "$(OUT)/obj" "$(OUT)/acnh-native"

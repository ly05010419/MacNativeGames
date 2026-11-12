# Local source-build checkpoint for the TMNT host. Generated headers and all
# outputs stay under WORK, outside the checkout. Runtime behavior is not yet
# validated; this target must not be packaged as a playable application.
WORK ?= /tmp/tmnt-port-build
OUT := $(WORK)/build
CLANG ?= clang
PY ?= python3
PKG_CONFIG ?= pkg-config
EXEFS ?=
REWRITTEN ?= $(WORK)/rewritten
ROMFS ?=
FONTS ?=
NRO_PREPARED ?= $(WORK)/nro-prepared
NRO_REWRITTEN ?= $(WORK)/nro-rewritten
NATIVE_OPT ?= -O2
CODEC_PACKAGES := opus vpx vorbis ogg
CODEC_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(CODEC_PACKAGES))
CODEC_ARCHIVES := $(foreach p,$(CODEC_PACKAGES),$(shell $(PKG_CONFIG) --variable=libdir $(p))/lib$(p).a)
GFX_SRC := $(wildcard third_party/d2r_gfx/gfx/nvn/*.c third_party/d2r_gfx/gfx/nvn/*.m third_party/d2r_gfx/gfx/nvn/*.S third_party/d2r_gfx/gfx/tex/*.c third_party/d2r_gfx/gfx/tex/*.m)
GFX_OBJECTS := $(patsubst %,$(OUT)/obj/%.o,$(GFX_SRC))
GFX_HEADERS := $(wildcard third_party/d2r_gfx/gfx/nvn/*.h third_party/d2r_gfx/gfx/nvn/include/*.h third_party/d2r_gfx/host/*.h)
RUNTIME_SRC := $(filter-out runtime/frontend_ios.m,$(wildcard runtime/*.c runtime/*.m runtime/*.S))
RUNTIME_HEADERS := $(wildcard runtime/*.h)
GENERATED := $(OUT)/runtime_tables.h $(OUT)/romfs_config.h $(OUT)/shared_fonts_config.h $(OUT)/tmnt_nro_digest.h

.PHONY: runtime prepare-tables prepare-resources prepare-nro probe-async-skip
probe-async-skip:
	@mkdir -p "$(OUT)"
	$(CLANG) -std=c11 -Wall -Wextra -Werror -Ithird_party/d2r_gfx/gfx/nvn/include tests/tmnt_async_skip_policy_test.c -o "$(OUT)/tmnt_async_skip_policy_test"
	"$(OUT)/tmnt_async_skip_policy_test"
	$(CLANG) -std=c11 -Wall -Wextra -Werror tests/tmnt_window_serial_set_test.c -o "$(OUT)/tmnt_window_serial_set_test"
	"$(OUT)/tmnt_window_serial_set_test"
	$(CLANG) -std=c11 -Wall -Wextra -Werror tests/tmnt_startup_present_policy_test.c -o "$(OUT)/tmnt_startup_present_policy_test"
	"$(OUT)/tmnt_startup_present_policy_test"
	$(CLANG) -std=c11 -Wall -Wextra -Werror tests/tmnt_romfs_read_exact_test.c -o "$(OUT)/tmnt_romfs_read_exact_test"
	"$(OUT)/tmnt_romfs_read_exact_test"

prepare-tables:
	@test -n "$(EXEFS)" || { echo 'set EXEFS to your verified private base ExeFS' >&2; exit 1; }
	@mkdir -p "$(OUT)"
	$(PY) scripts/module_metadata.py --exefs "$(EXEFS)" --rewritten "$(REWRITTEN)" --out "$(WORK)/metadata.json"
	$(PY) scripts/prepare_host_tables.py --exefs "$(EXEFS)" --rewritten "$(REWRITTEN)" --metadata "$(WORK)/metadata.json" --out "$(OUT)/runtime_tables.h"

prepare-resources:
	@test -n "$(ROMFS)" || { echo 'set ROMFS to your verified private RomFS directory' >&2; exit 1; }
	@mkdir -p "$(OUT)"
	$(PY) scripts/prepare_resources.py --romfs "$(ROMFS)" --out "$(WORK)/resources" $(if $(FONTS),--fonts "$(FONTS)")
	cp "$(WORK)/resources/romfs_config.h" "$(WORK)/resources/shared_fonts_config.h" "$(OUT)/"

prepare-nro:
	@test -n "$(ROMFS)" || { echo 'set ROMFS to your verified private RomFS directory' >&2; exit 1; }
	@mkdir -p "$(OUT)"
	$(PY) scripts/prepare_nro.py --romfs "$(ROMFS)" --out "$(NRO_PREPARED)"
	$(PY) scripts/rewrite_nro.py --prepared "$(NRO_PREPARED)" --out "$(NRO_REWRITTEN)"
	$(PY) scripts/prepare_nro_digest.py --prepared "$(NRO_PREPARED)" --rewritten "$(NRO_REWRITTEN)" --out "$(OUT)/tmnt_nro_digest.h"

$(GENERATED):
	@echo "missing private generated header $@: prepare verified user inputs first" >&2
	@false

$(OUT)/obj/%.o: % $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	$(CLANG) $(NATIVE_OPT) -g -Wall -Wextra -Wno-error -Wno-unused-function -Wno-unused-parameter -fobjc-arc -Ithird_party/d2r_gfx/gfx/nvn/include -c "$<" -o "$@"

$(OUT)/tmnt-native: $(RUNTIME_SRC) $(RUNTIME_HEADERS) $(GENERATED) $(GFX_OBJECTS) Makefile
	@$(PKG_CONFIG) --exists $(CODEC_PACKAGES) || { echo 'install the opus, libvpx, libvorbis and libogg development packages' >&2; exit 1; }
	@for archive in $(CODEC_ARCHIVES); do test -f "$$archive" || { echo "missing static codec archive: $$archive" >&2; exit 1; }; done
	$(CLANG) -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations \
	  -Ithird_party/d2r_gfx/gfx/nvn/include -I$(OUT) $(CODEC_CFLAGS) \
	  $(RUNTIME_SRC) $(GFX_OBJECTS) $(CODEC_ARCHIVES) \
	  -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics \
	  -framework CoreText -framework UniformTypeIdentifiers -fobjc-arc -framework Cocoa -framework CoreVideo -framework CoreFoundation \
	  -framework VideoToolbox -framework CoreMedia -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit -o "$@.new"
	mv "$@.new" "$@"

runtime: $(OUT)/tmnt-native

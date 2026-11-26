# 明星三缺一 2013 (a libnx homebrew) on the Zelda-line runtime: the rewriter, the host gate, threads and IPC from
# games/totk and shared/ (third_party/COPIES.md); no NVN graphics layer (the game draws in software).
# Game files and generated outputs stay in the work folder (WORK, outside the checkout).
WORK ?= $(error Set WORK to the work folder (README.md))
OUT := $(WORK)/build
PY ?= python3
NATIVE_OPT ?= -O2

RUNTIME_SRC := $(wildcard runtime/*.c runtime/*.m runtime/*.S) third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c third_party/d2r_gfx/gfx/nvn/nvn_tex.c
RUNTIME_HEADERS := $(wildcard runtime/*.h) third_party/libnx/hid_shmem.h

.PHONY: prepare runtime probe boot test clean app dist
prepare:
	"$(PY)" scripts/prepare_mj2013.py --work "$(WORK)" --py "$(PY)" $(if $(GAME),--game "$(GAME)") $(if $(KEYS),--keys "$(KEYS)")

$(OUT)/runtime_tables.h:
	@echo "missing $@: run make prepare" && false

# The game binary and its launcher run on macOS 26 whatever SDK builds them: with no minimum, clang and ld take the
# SDK's version (Launcher 1.1.5, made on macOS 27, carried runtimes of minos 27.2 that macOS 26 refused, #2395). Set only on
# these targets (make passes it to what they are made of): the translator and spirv-cross targets keep their own flags.
$(OUT)/mj2013-native: export MACOSX_DEPLOYMENT_TARGET := 26.0
$(OUT)/mj2013-native: $(RUNTIME_SRC) $(RUNTIME_HEADERS) $(OUT)/runtime_tables.h runtime/game_info.plist Makefile
	xcrun clang -std=gnu11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -Wno-unused-parameter \
	  -Wl,-sectcreate,__TEXT,__info_plist,runtime/game_info.plist -I$(OUT) -Iruntime $(RUNTIME_SRC) -fobjc-arc \
	  -framework Metal -framework QuartzCore -framework Foundation -framework Cocoa -framework CoreGraphics -framework ImageIO \
	  -framework CoreFoundation -framework AudioToolbox -o "$@"

runtime: $(OUT)/mj2013-native

probe: runtime
	cd "$(WORK)" && ./build/mj2013-native rewritten/v0 probe

boot: runtime
	cd "$(WORK)" && ./build/mj2013-native rewritten/v0 boot

test:
	python3 -m unittest discover -s tests -t . -v

# The Mac app (scripts/package_app.py, tools/port_app/bundle.py): the runtime, the rewritten main and the RomFS image as
# APFS clones inside 明星三缺一.app; the player's data in ~/Library/Application Support/MacNativeGames/mj2013 (app/launcher.c).
# ICON=<.icns or image> gives it an icon.
APP ?= $(abspath $(WORK)/..)/mj2013-app/明星三缺一.app
ICON ?=
app: runtime
	"$(PY)" scripts/package_app.py --work "$(WORK)" --output "$(APP)" --record-work "$(WORK)" $(if $(ICON),--icon "$(ICON)")
dist: app
	@echo "dist ready: $(APP)"

clean:
	rm -f "$(OUT)/mj2013-native"

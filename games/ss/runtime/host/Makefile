# Game profile (dm/games/<id>/): the host is built for it (make GAME=base game title ID, required: there is no default game). game_profile.h is generated from game.tsv into gen/<id>/; the chain version header dmsh_chain.h (one for every game) lives in dm/gfx/nvn
# without GAME only clean passes; anything else (a bare make too, whose default goal is the host) stops and lists the profiles
ifeq ($(strip $(GAME)),)
ifneq ($(filter-out clean,$(or $(MAKECMDGOALS),dmhost)),)
$(error GAME is not set: make GAME=<base game title ID>: $(shell python3 ../games/game.py list | awk -F'\t' '{printf "%s (%s)  ", $$1, $$3}'))
endif
endif
# A profile outside the repository (DM_GAMES_DIR, e.g. the NSP script's provisional profile) is reached through the symlink gen/<id>/profile: make cannot take paths with spaces.
# DM_GAMES_FIRST=1 (the NSP script's --provisional) takes it even when dm/games has the game
ifneq ($(strip $(GAME)),)
GAME_DIR := $(if $(and $(wildcard ../games/$(GAME)/game.tsv),$(if $(DM_GAMES_FIRST),,1)),../games/$(GAME),$(shell python3 ../games/game.py link $(GAME) gen/$(GAME)/profile >&2 && echo gen/$(GAME)/profile))
ifeq ($(GAME_DIR),)
$(error no game profile $(GAME) (neither in dm/games nor in DM_GAMES_DIR))
endif
endif
GAME_H = gen/$(GAME)/game_profile.h
include ../mac/min_os.mk
CFLAGS=-arch arm64 -O1 -g -Wall -Wno-unused-function -mmacosx-version-min=$(MACOS_MIN)
# Service-layer takeover hle_*.c files are included by wildcard: adding a file does not touch this line (avoids merge conflicts)
SRC=main.c loader.c hle.c crash.c probe.c thread.c save_sync.c memstat.m haptics_apple.m $(sort $(wildcard hle_*.c)) front_mac.m front_settings.m front_hud.m pso_export.m entry.S
# NVN null device + nn::vi + the Vulkan layer (dm/gfx/)
# Sources under dm/gfx, dm/gfx/nvn, dm/gfx/tex, dm/gfx/vk and dm/gfx/gl are included by wildcard (tests/ and metal/ do not go into the host): adding a file does not touch this line
GFX=$(sort $(wildcard ../gfx/*.c ../gfx/nvn/*.c ../gfx/nvn/*.m ../gfx/nvn/*.S ../gfx/tex/*.c ../gfx/tex/*.m ../gfx/vk/*.m ../gfx/gl/*.m))
GFX_H=$(sort $(wildcard ../gfx/nvn/*.h ../gfx/nvn/*.inc ../gfx/nvn/include/*.h ../gfx/tex/*.h))   # *.inc: nvn_mtl.m includes them (nvn_geometry.inc); an edit there alone used not to rebuild the host
# Third party: libvpx ships the VP8 and VP9 decoder sources (movie decoding, hle_movie.c), with its configuration headers in config/ (arm64, shared by Mac and iOS). Built separately into a static library, file by file, so host changes do not recompile it
VPX=../third_party/libvpx
VPX_SRC=$(sort $(shell find $(VPX) -name '*.c'))
VPX_OBJ=$(patsubst $(VPX)/%.c,gen/vpx/%.o,$(VPX_SRC))
VPX_LIB=gen/vpx/libvpx.a
gen/vpx/%.o: $(VPX)/%.c $(wildcard $(VPX)/config/*.h) ../mac/min_os.mk
	@mkdir -p $(dir $@)
	clang -arch arm64 -O2 -g -mmacosx-version-min=$(MACOS_MIN) -I$(VPX) -I$(VPX)/config -c $< -o $@
$(VPX_LIB): $(VPX_OBJ)
	rm -f $@; ar rcs $@ $^
# Third party: dav1d, the AV1 decoder where VideoToolbox has none (hle_movie_av1.c), with its arm64 NEON assembly and its configuration in config/. Its *_tmpl.c files are built
# through the per-bit-depth wrappers in config/tmpl, and the assembly files other assembly files include (*_tmpl.S, util.S) are not built on their own: the list meson builds
DAV1D=../third_party/dav1d
DAV1D_SRC=$(sort $(filter-out %_tmpl.c,$(wildcard $(DAV1D)/src/*.c)) $(DAV1D)/src/arm/cpu.c $(wildcard $(DAV1D)/config/tmpl/*.c) \
          $(filter-out %_tmpl.S %/util.S,$(wildcard $(DAV1D)/src/arm/64/*.S)))
DAV1D_OBJ=$(patsubst $(DAV1D)/%,gen/dav1d/%.o,$(DAV1D_SRC))
DAV1D_LIB=gen/dav1d/libdav1d.a
DAV1D_H=$(wildcard $(DAV1D)/config/*.h $(DAV1D)/src/*.h $(DAV1D)/src/*_tmpl.c $(DAV1D)/src/arm/*.h $(DAV1D)/src/arm/asm.S $(DAV1D)/src/arm/64/*_tmpl.S $(DAV1D)/src/arm/64/util.S $(DAV1D)/include/*/*.h)
gen/dav1d/%.o: $(DAV1D)/% $(DAV1D_H) ../mac/min_os.mk
	@mkdir -p $(dir $@)
	clang -O2 -g -mmacosx-version-min=$(MACOS_MIN) -fvisibility=hidden -fomit-frame-pointer -fno-align-functions -I$(DAV1D) -I$(DAV1D)/include -I$(DAV1D)/config -c $< -o $@
$(DAV1D_LIB): $(DAV1D_OBJ)
	rm -f $@; ar rcs $@ $^
# The host's own Metal shaders (dm/gfx/msl/*.metal) compiled for MSL_SDK into gen/msl-<sdk>/host_msl_gen.h (dm/gfx/tools/host_msl.py; dm/gfx/nvn/host_msl.m loads them), so the host
# compiles no MSL at run time. The iOS build (dm/ios/tools/gen_build.py) makes it with MSL_SDK=iphoneos, for the app's deployment target (MSL_MIN_iphoneos, as project.yml)
MSL_SDK ?= macosx
MSL_MIN_macosx = $(MACOS_MIN)
MSL_MIN_iphoneos = 26.0
MSL_SRC = $(sort $(wildcard ../gfx/msl/*.metal))
MSL_H = gen/msl-$(MSL_SDK)/host_msl_gen.h
# Recheck the toolchain and target on every build; the generator keeps the header unchanged when its inputs match.
$(MSL_H): $(MSL_SRC) ../gfx/tools/host_msl.py FORCE
	python3 ../gfx/tools/host_msl.py --sdk $(MSL_SDK) $(if $(MSL_MIN_$(MSL_SDK)),--min $(MSL_MIN_$(MSL_SDK))) $@ $(MSL_SRC)
.PHONY: host_msl
host_msl: $(MSL_H)
.DEFAULT_GOAL := dmhost   # The header generation rules are written first below, so the default goal must be stated explicitly (otherwise make only generates the headers and does not build the host)
.PHONY: game_profile
game_profile: $(GAME_H)
# Regenerated on every make (FORCE): the same GAME can come from another profile folder (DM_GAMES_FIRST), whose game.tsv may be older than the header; game.py writes it only when it changes
$(GAME_H): $(GAME_DIR)/game.tsv ../games/game.py FORCE
	@python3 ../games/game.py header $(GAME) $@
# Records which game the host was built for: changing GAME changes the stamp's content and rebuilds the host (otherwise make sees dmhost as newer than both headers and keeps the other game's host).
# The game of the last build is also read when the Makefile is parsed, and a different one forces the link: make compares whole seconds, and a stamp written in the same second as
# another game's dmhost (two builds in a row) did not count as newer, so that game's host was kept
GAME_STAMP = gen/game.stamp
$(GAME_STAMP): FORCE
	@mkdir -p gen; echo $(GAME) | cmp -s - $@ || echo $(GAME) > $@
ifneq ($(strip $(GAME)),)
ifneq ($(shell cat $(GAME_STAMP) 2>/dev/null),$(GAME))
dmhost: FORCE
endif
endif
.PHONY: FORCE
FORCE:
dmhost: $(SRC) $(GFX) ../third_party/stb_vorbis/stb_vorbis.c dm.h hle.h romfs_image.h save_sync.h dm_input.h pad_slots.h hle_haptics.h haptics_apple.h motion_fusion.h motion_apple.h motion_pro.h motion_pro_mac.h pad_apple.h pad_hid.h pad_hid_mac.h front.h front_settings.h front_shortcuts.h shader_activity.h hud_settings.h front_hud.h front_graph.h pso_export.h mac_display_settings.h $(GFX_H) $(GAME_H) $(GAME_STAMP) $(VPX_LIB) $(DAV1D_LIB) $(MSL_H)
	clang $(CFLAGS) -I../gfx/nvn/include -I$(dir $(MSL_H)) -I../third_party/vulkan-headers/include -I$(dir $(GAME_H)) -I$(VPX) -I$(DAV1D)/include -I../third_party/stb_vorbis -fobjc-arc -o $@ $(SRC) $(GFX) $(VPX_LIB) $(DAV1D_LIB) -framework Cocoa -framework QuartzCore -framework Metal -framework MetalFX -framework GameController -framework CoreHaptics -framework IOKit -framework AudioToolbox -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework ImageIO -framework UniformTypeIdentifiers
clean:   # the host and everything make generates for it (gen/: the profile headers, the game stamp and the libvpx and dav1d builds; dmhost_* and *.dSYM beside them, as in .gitignore)
	rm -rf gen dmhost dmhost_* *.dSYM
.PHONY: clean

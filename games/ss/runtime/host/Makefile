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
# Third party: libvpx (VP8 and VP9 movies, hle_movie.c) and dav1d (AV1 where VideoToolbox has none, hle_movie_av1.c), static libraries from MacNativeGames'
# tools/deps (tools/deps/pins.py), made once per Mac from pinned sources. The build passes their prefixes (scripts/ss_env.py); a make in the checkout asks tools/deps.
# Expanded only in the link recipe, so a make of another target (game_profile, host_msl, clean) needs neither
DEPS = $(firstword $(wildcard $(MNG_REPO)/tools/deps ../../../../tools/deps))
VPX_PREFIX ?= $(or $(and $(DEPS),$(shell python3 $(DEPS) prefix libvpx)),$(error VPX_PREFIX is not set and no MacNativeGames tools/deps was found))
DAV1D_PREFIX ?= $(or $(and $(DEPS),$(shell python3 $(DEPS) prefix dav1d)),$(error DAV1D_PREFIX is not set and no MacNativeGames tools/deps was found))
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
dmhost: $(SRC) $(GFX) ../third_party/stb_vorbis/stb_vorbis.c dm.h hle.h romfs_image.h save_sync.h dm_input.h pad_slots.h hle_haptics.h haptics_apple.h motion_fusion.h motion_apple.h motion_pro.h motion_pro_mac.h pad_apple.h pad_hid.h pad_hid_mac.h front.h front_settings.h front_shortcuts.h shader_activity.h hud_settings.h front_hud.h front_graph.h pso_export.h mac_display_settings.h $(GFX_H) $(GAME_H) $(GAME_STAMP) $(MSL_H)
	clang $(CFLAGS) -I../gfx/nvn/include -I$(dir $(MSL_H)) -I../third_party/vulkan-headers/include -I$(dir $(GAME_H)) -I$(VPX_PREFIX)/include -I$(DAV1D_PREFIX)/include -I../third_party/stb_vorbis -fobjc-arc -o $@ $(SRC) $(GFX) $(VPX_PREFIX)/lib/libvpx.a $(DAV1D_PREFIX)/lib/libdav1d.a -framework Cocoa -framework QuartzCore -framework Metal -framework MetalFX -framework GameController -framework CoreHaptics -framework IOKit -framework AudioToolbox -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework ImageIO -framework UniformTypeIdentifiers
clean:   # the host and everything make generates for it (gen/: the profile headers and the game stamp; dmhost_* and *.dSYM beside them, as in .gitignore)
	rm -rf gen dmhost dmhost_* *.dSYM
.PHONY: clean

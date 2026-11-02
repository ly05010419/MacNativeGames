# Texture sub-module unit tests (seconds): make run
CFLAGS = -O2 -g -Wall -Wextra
# SAN: extra compiler flags, O: the output folder (with a trailing slash) of a sanitizer build (make asan below sets both); both empty otherwise
SAN ?=
O ?=
CLANG = clang $(SAN)
$(O)t_tex: t_tex.c ../nvn_deswizzle.c ../nvn_deswizzle.h ../../nvn/nvn_tex.c ../../nvn/nvn_tex.h
	$(CLANG) $(CFLAGS) -o $@ t_tex.c ../nvn_deswizzle.c ../../nvn/nvn_tex.c -lm
$(O)t_fmt_mtl: t_fmt_mtl.m ../nvn_fmt_mtl.c ../nvn_fmt_mtl.h
	$(CLANG) $(CFLAGS) -fobjc-arc -o $@ t_fmt_mtl.m ../nvn_fmt_mtl.c -framework Metal -framework Foundation
run: t_tex t_fmt_mtl
	./t_fmt_mtl && ./t_tex
.PHONY: run
# ASan and UBSan (../../../sanitizers/sanitizers.mk): the CPU test t_tex, built into asan/ (t_fmt_mtl creates Metal textures and stays local)
include ../../../sanitizers/sanitizers.mk
.PHONY: asan
asan:
	@$(call san_fresh,asan,$(ASAN_FLAGS))
	$(MAKE) O=asan/ SAN="$(ASAN_FLAGS)" asan/t_tex
	$(ASAN_ENV) ./asan/t_tex

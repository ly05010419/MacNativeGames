# Texture sub-module unit tests (seconds): make run
CFLAGS = -O2 -g -Wall -Wextra
t_tex: t_tex.c ../nvn_deswizzle.c ../nvn_deswizzle.h ../../nvn/nvn_tex.c ../../nvn/nvn_tex.h
	clang $(CFLAGS) -o $@ t_tex.c ../nvn_deswizzle.c ../../nvn/nvn_tex.c -lm
t_fmt_mtl: t_fmt_mtl.m ../nvn_fmt_mtl.c ../nvn_fmt_mtl.h
	clang $(CFLAGS) -fobjc-arc -o $@ t_fmt_mtl.m ../nvn_fmt_mtl.c -framework Metal -framework Foundation
run: t_tex t_fmt_mtl
	./t_fmt_mtl && ./t_tex
.PHONY: run

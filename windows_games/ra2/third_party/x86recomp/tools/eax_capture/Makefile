# eaxcap.exe for Windows 98 SE and later, cross-compiled with mingw-w64.
# No C runtime startup code (mainCRTStartup is ours), so the only imports
# are the ones eaxcap.c and cases.c call; check_imports.py fails the build
# on anything Windows 98 SE lacks.
#
#   make            build/eaxcap.exe, then the import check
#   make check      the import check alone

CC := i686-w64-mingw32-gcc
OBJDUMP := i686-w64-mingw32-objdump
BUILD := build

CFLAGS := -std=gnu99 -O2 -march=i686 -mtune=generic -Wall -Wextra \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-ident \
	-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
	-D__USE_MINGW_ANSI_STDIO=0 -DWINVER=0x0400 -D_WIN32_WINDOWS=0x0410 \
	-D_WIN32_WINNT=0x0400 -DDIRECTSOUND_VERSION=0x0700
LDFLAGS := -nostdlib -nostartfiles -Wl,-e,_mainCRTStartup -s \
	-Wl,--subsystem,console:4.0 -Wl,--major-os-version,4 -Wl,--minor-os-version,0 \
	-Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--disable-reloc-section
LIBS := -lmsvcrt -lkernel32 -luser32 -lwinmm -ldsound -lmingwex -lmingw32 -lmsvcrt -lgcc

all: $(BUILD)/eaxcap.exe check

$(BUILD)/eaxcap.exe: eaxcap.c cases.c cases.h Makefile
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ eaxcap.c cases.c $(LDFLAGS) $(LIBS)

check: $(BUILD)/eaxcap.exe
	python3 -I check_imports.py --objdump $(OBJDUMP) --allow win98_imports.txt $<

clean:
	rm -rf $(BUILD)

.PHONY: all check clean

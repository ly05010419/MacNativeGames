CC      = clang
CFLAGS  = -O2 -Wall -Wextra -std=c11
OBJCFLAGS = -O2 -Wall -Wextra -fobjc-arc
LDLIBS  = -framework Metal -framework Foundation

all: test_offscreen

mtl_backend.o: mtl_backend.m mtl_backend.h
	$(CC) $(OBJCFLAGS) -c -o $@ $<

test_offscreen.o: test_offscreen.c mtl_backend.h
	$(CC) $(CFLAGS) -c -o $@ $<

test_offscreen: test_offscreen.o mtl_backend.o
	$(CC) -o $@ $^ $(LDLIBS)

run: test_offscreen
	./test_offscreen

clean:
	rm -f test_offscreen *.o

.PHONY: all run clean

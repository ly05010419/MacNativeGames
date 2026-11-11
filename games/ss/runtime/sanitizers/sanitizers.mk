# Sanitizer builds of the unit tests, shared by dm/host/tests and dm/gfx/tex/tests (make asan, and make tsan-threads in dm/host/tests; CI runs them all).
# A second make compiles the same test rules with SAN (the extra compiler flags) into a build folder of its own, so the plain build stays as it is.
# Exceptions for test fixtures and uninstrumented framework synchronization are documented beside this file:
# ignorelist.txt (ASan and UBSan, at compile time) and tsan.supp (TSan, at run time). New host findings are fixed.
SAN_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
ASAN_FLAGS = -g -fsanitize=address,undefined -fno-omit-frame-pointer -fsanitize-ignorelist=$(SAN_DIR)/ignorelist.txt
# use_sigaltstack=0: the host's crash.c gives every thread an alternate signal stack of its own, which ASan would unmap as its own at thread exit
ASAN_ENV = ASAN_OPTIONS=halt_on_error=1:detect_stack_use_after_return=1:use_sigaltstack=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
# -fno-inline: every function keeps its own frame, so tsan.supp's entries match on every toolchain (Xcode 26.6 on GitHub's runner reported an inlined callee's
# write under its caller, and the entry for the callee did not match); TSan judges the order of accesses, not their timing
TSAN_FLAGS = -g -fsanitize=thread -fno-inline
TSAN_ENV = TSAN_OPTIONS=suppressions=$(SAN_DIR)/tsan.supp
# The test rules do not list the flags or the ignorelist: $(call san_fresh,<folder>,<flags>) empties a sanitizer build folder whose stamp differs, so a change rebuilds its tests
san_fresh = mkdir -p $(1) && { echo '$(2)'; cat $(SAN_DIR)/ignorelist.txt; } | cmp -s - $(1)/.san || { rm -rf $(1)/t_*; { echo '$(2)'; cat $(SAN_DIR)/ignorelist.txt; } > $(1)/.san; }

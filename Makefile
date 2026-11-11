# Source gate for this repository: fast, offline, and it changes no source file.
# It mirrors .github/workflows/tests.yml, so a commit that passes here passes CI.
#
#   make check
#
# The Python tests need the packages in games/totk/requirements.txt; install them once
# (the CI does the same with pip) and this stays offline.

# The tests import compression.zstd (games/totk/scripts/make_romfs_patches.py), which is standard only from Python 3.14;
# the README and the CI use that version, so prefer it when it is installed. PYTHON still overrides this.
PYTHON ?= $(shell command -v python3.14 >/dev/null 2>&1 && echo python3.14 || echo python3)
CC ?= cc

# UBSan flags shared by every C unit test. CI runs the same builds via `make check-c`.
C_TEST_CFLAGS = -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all

# Single list for `make check` and CI. Each stem builds tests/test_<stem>.c unless
# C_TEST_SRC_<stem> overrides the source; C_TEST_EXTRA_<stem> appends compile args.
C_TESTS = \
	romfs_paths \
	feedback_settings \
	controller_map \
	keyboard_map \
	users \
	game_language \
	audio_ramp \
	bow_state \
	screenshot \
	screenshot_botw \
	agent_slot_limit \
	agent_slot_limit_oot
C_TEST_EXTRA_users = games/totk/runtime/account.c -Igames/totk/runtime
C_TEST_SRC_screenshot_botw = tests/test_screenshot.c
C_TEST_EXTRA_screenshot_botw = -DGAME_BOTW190
C_TEST_SRC_agent_slot_limit_oot = tests/test_agent_slot_limit.c
C_TEST_EXTRA_agent_slot_limit_oot = -DAGENT_SLOT_LIMIT_H_PATH='"../games/oot/runtime/agent_slot_limit.h"'

.PHONY: check check-c

check:
	$(PYTHON) -m unittest discover -s tests -q
	@$(MAKE) --no-print-directory check-c

check-c:
	@tmp=$$(mktemp -d) && trap 'rm -rf "$$tmp"' EXIT; \
	$(foreach t,$(C_TESTS),$(CC) $(C_TEST_CFLAGS) $(or $(C_TEST_SRC_$(t)),tests/test_$(t).c) $(C_TEST_EXTRA_$(t)) -o "$$tmp/test-$(t)" && "$$tmp/test-$(t)" && )true

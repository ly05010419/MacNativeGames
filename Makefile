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

.PHONY: check

check:
	$(PYTHON) -m unittest discover -s tests -q
	@tmp=$$(mktemp -d) && trap 'rm -rf "$$tmp"' EXIT; \
	 $(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all \
	   tests/test_romfs_paths.c -o "$$tmp/test-romfs-paths" && "$$tmp/test-romfs-paths" && \
	 $(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all \
	   tests/test_controller_map.c -o "$$tmp/test-controller-map" && "$$tmp/test-controller-map" && \
	 $(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all \
	   tests/test_keyboard_map.c -o "$$tmp/test-keyboard-map" && "$$tmp/test-keyboard-map"

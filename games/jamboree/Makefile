PYTHON ?= python3
.PHONY: help build test test-gpu
help:
	@$(PYTHON) scripts/build_jamboree.py --help
build:
	@test -n "$(INPUTS)" -a -n "$(KEYS)" -a -n "$(LIBHAC)" -a -n "$(WORK)" -a -n "$(OUTPUT)" || (echo 'Set INPUTS, KEYS, LIBHAC, WORK, OUTPUT; see README.md'; exit 1)
	$(PYTHON) scripts/build_jamboree.py "$(INPUTS)" --keys "$(KEYS)" --libhac-package "$(LIBHAC)" --work-dir "$(WORK)" --output "$(OUTPUT)" --language "$(or $(LANGUAGE),en-US)"
test:
	$(PYTHON) -m unittest discover -s tests -v
	$(PYTHON) -m unittest discover -s runtime/tools/tests -v
test-gpu:
	@test -n "$(WORK)" || (echo 'Set WORK to a private directory outside the checkout'; exit 1)
	$(PYTHON) scripts/test_gpu.py --work-dir "$(WORK)"
.PHONY: test-ui
test-ui:
	@test -n "$(WORK)" || (echo 'Set WORK to a private directory outside the checkout'; exit 1)
	$(PYTHON) scripts/test_settings.py --work-dir "$(WORK)"

# Line coverage of the unit tests, shared by dm/host/tests and dm/gfx/tex/tests (make coverage in each; CI's coverage job runs both, dm/tools/coverage_report.py summarizes them).
# As for the sanitizers (sanitizers.mk), a second make compiles the same test rules with SAN = COV_FLAGS into a build folder of its own, so the plain build stays as it is.
# Every test process writes a raw profile of its own (LLVM_PROFILE_FILE, %p) in continuous mode (%c: the counters are mapped onto the file as they run), so the children
# that a test forks and ends with _exit, which writes no profile, count into their parent's file; llvm-profdata merges the profiles and llvm-cov exports the line counts as lcov, for the sources the
# tests compile (their includes too), without the tests themselves, third-party code, generated headers and the SDK. Apple's toolchain has both tools (xcrun).
COV_FLAGS = -fprofile-instr-generate -fcoverage-mapping
COV_IGNORE = /tests/|/third_party/|/gen/|^/Applications/|^/Library/
# $(call cov_profile,<folder>): the LLVM_PROFILE_FILE pattern of a coverage build
cov_profile = $(abspath $(1))/prof/%c%p.profraw
# $(call cov_export,<folder>): merges the profiles in <folder>/prof (but empty ones, of a process killed before it wrote any) over the tests built in <folder> (t_*) into
# <folder>/coverage.lcov, whose DA lines are llvm-cov's own line counts (those `llvm-cov show` marks), and prints llvm-cov report's total line (it adds up functions,
# so a line two functions share counts twice there: the summary's per-line counts can differ by a few lines)
cov_export = find $(1)/prof -name '*.profraw' -size +0 | xargs xcrun llvm-profdata merge -sparse -o $(1)/cov.profdata && \
	set -- $$(for f in $(1)/t_*; do [ -f "$$f" ] && [ -x "$$f" ] && echo "-object=$$f"; done) && \
	xcrun llvm-cov export -format=lcov -skip-functions -instr-profile=$(1)/cov.profdata -ignore-filename-regex='$(COV_IGNORE)' "$$@" > $(1)/coverage.lcov && \
	xcrun llvm-cov report -instr-profile=$(1)/cov.profdata -ignore-filename-regex='$(COV_IGNORE)' "$$@" | tail -1 && \
	echo "coverage: $(abspath $(1))/coverage.lcov (the summary: python3 dm/tools/coverage_report.py --host or --textures <that file>)"

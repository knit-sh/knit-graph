## Test suite. Each script exits 0 on success, non-zero on failure.

# Make the built binary and the source dir available to the scripts.
AM_TESTS_ENVIRONMENT = \
	KG='$(abs_top_builddir)/src/knit-graph'; export KG; \
	srcdir='$(srcdir)'; export srcdir;

# Run *.sh tests through the shell so they need no execute bit.
TEST_EXTENSIONS = .sh
SH_LOG_COMPILER = $(SHELL)

TESTS = \
	valid.sh \
	invalid.sh \
	ast_golden.sh \
	catalog.sh \
	explain.sh \
	memcheck.sh

# Ship the tests and their fixtures in the distribution tarball.
# fixture.sh is sourced by catalog.sh/memcheck.sh, so it ships but is not a test.
EXTRA_DIST = \
	valid.sh \
	invalid.sh \
	ast_golden.sh \
	catalog.sh \
	explain.sh \
	memcheck.sh \
	fixture.sh \
	golden

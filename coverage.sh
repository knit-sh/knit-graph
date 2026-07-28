#!/bin/sh
# coverage.sh -- measure line coverage of the hand-written sources and enforce a
# minimum threshold. Run against a build tree configured with --enable-coverage,
# after "make check" has produced the .gcda data.
#
#   ./coverage.sh [BUILDDIR] [THRESHOLD]
#
# BUILDDIR defaults to "build", THRESHOLD to 80 (percent of lines). The generated
# bison/flex sources (cypher_parser.*, cypher_scanner.*) and system headers are
# excluded so the figure reflects only code we wrote.
#
# Note: this uses "lcov --summary" for the aggregate, not "lcov --list": lcov 2.0
# renders a correct .info file and a correct summary, but its per-file --list
# table misreports hit counts for gcov 13's intermediate format. If genhtml is
# available an HTML report is written to BUILDDIR/coverage-html for browsing.

set -eu

builddir=${1:-build}
threshold=${2:-80}

if ! command -v lcov >/dev/null 2>&1; then
	echo "coverage.sh: lcov is required (apt-get install lcov)" >&2
	exit 2
fi
if [ ! -d "$builddir/src" ]; then
	echo "coverage.sh: '$builddir/src' not found; configure with --enable-coverage and build first" >&2
	exit 2
fi

raw="$builddir/coverage.info"
filtered="$builddir/coverage.filtered.info"

# geninfo_unexecuted_blocks=1 silences gcov 13's "unexecuted block on non-branch
# line" notices (harmless inlining artifacts). --ignore-errors unused keeps the
# remove step from failing if a pattern matches nothing on some toolchain.
lcov --quiet --capture --directory "$builddir/src" \
	--rc geninfo_unexecuted_blocks=1 \
	--output-file "$raw"
lcov --quiet --remove "$raw" \
	'*/cypher_parser.c' '*/cypher_parser.y' \
	'*/cypher_scanner.c' '*/cypher_scanner.l' \
	'/usr/*' \
	--ignore-errors unused \
	--output-file "$filtered"

# Human-readable summary (correct in lcov 2.0, unlike --list).
lcov --summary "$filtered"

# Optional browsable report.
if command -v genhtml >/dev/null 2>&1; then
	genhtml --quiet --output-directory "$builddir/coverage-html" "$filtered" || true
fi

# Enforce the threshold from the exact hit/total counts in the summary line
# ("  lines......: 89.3% (1731 of 1938 lines)") to avoid float rounding.
summary=$(lcov --summary "$filtered" 2>&1 | grep -E '^[[:space:]]*lines')
hit=$(printf '%s\n' "$summary" | sed -n 's/.*(\([0-9]*\) of \([0-9]*\).*/\1/p')
total=$(printf '%s\n' "$summary" | sed -n 's/.*(\([0-9]*\) of \([0-9]*\).*/\2/p')

if [ -z "$hit" ] || [ -z "$total" ] || [ "$total" -eq 0 ]; then
	echo "coverage.sh: could not parse coverage summary" >&2
	exit 2
fi

awk -v h="$hit" -v t="$total" -v th="$threshold" 'BEGIN {
	p = 100 * h / t
	printf "line coverage: %.1f%% (%d/%d), threshold %s%%\n", p, h, t, th
	if (p + 1e-9 < th) {
		printf "FAIL: coverage %.1f%% is below the %s%% threshold\n", p, th
		exit 1
	}
	print "OK: coverage meets the threshold"
}'

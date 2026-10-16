#!/usr/bin/env bash
# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT
#
# Build and run Hootedit's tests. Same command line as fleetwm's scripts/test.sh and Lestrix's native/test.sh.
#
#   scripts/test.sh                  everything: unit, perf, ui, static (one line per group + summary)
#   scripts/test.sh unit             a group: unit | perf | ui | static
#   scripts/test.sh lint vim         one or more areas of the unit tests (see --areas)
#   scripts/test.sh -f 'Lint.K8s*'   any gtest filter
#   scripts/test.sh -v lint          verbose: every test case
#   scripts/test.sh -l               list every unit test case
#   scripts/test.sh --areas          list the area names and what they cover
#   scripts/test.sh -r 20 --shuffle  repeat the unit tests 20 times in random order (finds order dependence)
#   scripts/test.sh --xml out.xml    also write a JUnit-style report of the unit tests
#   scripts/test.sh --no-build       just run the existing binaries
#
# Groups: unit (GoogleTest under ASan and UBSan; includes the differential tests against std::string and std::regex and the
# random-input tests), perf (the speed gate), ui (the real window driven by --script, under xvfb-run), static (scripts parse, doc links
# resolve). Uses build-test/ next to the sources (override with BUILD_DIR=...). The exit status is the test result.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-test}"
declare -A AREAS=(
  [buffer]='Buffer.*' [doc]='Doc.*' [syntax]='Syntax.*' [fuzzy]='Fuzzy.*' [diff]='Diff.*' [merge]='Merge.*' [regex]='Regex.*'
  [vim]='Vim.*' [outline]='Outline.*' [tree]='Tree.*' [lint]='Lint.*' [preview]='Preview.*'
)
declare -A WHAT=(
  [buffer]='piece table against std::string, hot-reload safety' [doc]='undo/redo, save tracking, reload from disk' [syntax]='lexer classes and language detection'
  [fuzzy]='fuzzy matcher ranking' [diff]='line diff: minimal and replayable' [merge]='three-way merge parsing' [regex]='own regex engine against std::regex'
  [vim]='vim engine' [outline]='symbol outline scan' [tree]='Explorer tree, compact folders, ghosts' [lint]='live checks: Ansible, Kubernetes, shell, ...' [preview]='documentation preview parsing'
)
BUILD=1; VERBOSE=0; LIST=0; REPEAT=1; SHUFFLE=0; XML=""; FILTER=""; GROUPS_WANTED=(); AREAS_WANTED=()
while [ $# -gt 0 ]; do
  case "$1" in
    unit|perf|ui|static) GROUPS_WANTED+=("$1") ;;
    --areas) for a in $(printf '%s\n' "${!AREAS[@]}" | sort); do printf '  %-8s %-14s %s\n' "$a" "${AREAS[$a]}" "${WHAT[$a]}"; done; exit 0 ;;
    -f) FILTER="$2"; shift ;;
    -v) VERBOSE=1 ;;
    -l) LIST=1 ;;
    -r) REPEAT="$2"; shift ;;
    --shuffle) SHUFFLE=1 ;;
    --xml) XML="$2"; shift ;;
    --no-build) BUILD=0 ;;
    -h|--help) sed -n '5,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) if [ -n "${AREAS[$1]:-}" ]; then AREAS_WANTED+=("$1"); else echo "unknown option or area: $1 (see --areas)" >&2; exit 2; fi ;;
  esac
  shift
done
if [ ${#AREAS_WANTED[@]} -gt 0 ] || [ -n "$FILTER" ] || [ "$LIST" = 1 ]; then GROUPS_WANTED=(unit); fi
[ ${#GROUPS_WANTED[@]} -gt 0 ] || GROUPS_WANTED=(unit perf ui static)

if [ "$BUILD" = 1 ]; then
  meson setup "$BUILD_DIR" "$ROOT" --buildtype release --reconfigure >/dev/null 2>&1 || meson setup "$BUILD_DIR" "$ROOT" --buildtype release >/dev/null || exit 1
  ninja -C "$BUILD_DIR" hootedit hootedit-unit-tests hootedit-perf-gate >/dev/null || { echo "build failed"; exit 1; }
fi

declare -a NAMES RESULTS
record() { NAMES+=("$1"); RESULTS+=("$2"); }

run_unit() {
  local f="$FILTER"
  if [ ${#AREAS_WANTED[@]} -gt 0 ]; then f=""; for a in "${AREAS_WANTED[@]}"; do f+="${f:+:}${AREAS[$a]}"; done; fi
  local args=(); [ -n "$f" ] && args+=("--gtest_filter=$f"); [ "$LIST" = 1 ] && args+=(--gtest_list_tests)
  [ "$REPEAT" -gt 1 ] && args+=("--gtest_repeat=$REPEAT"); [ "$SHUFFLE" = 1 ] && args+=(--gtest_shuffle)
  [ -n "$XML" ] && args+=("--gtest_output=xml:$XML")
  if [ "$VERBOSE" = 1 ] || [ "$LIST" = 1 ]; then "$BUILD_DIR/hootedit-unit-tests" "${args[@]}"; return; fi
  local out; out=$("$BUILD_DIR/hootedit-unit-tests" "${args[@]}" 2>&1); local rc=$?
  printf '%s\n' "$out" | grep -E "^\[  (PASSED|FAILED)  \]|^\[==========\] [0-9]+ tests? from" | tail -3
  [ $rc != 0 ] && printf '%s\n' "$out" | grep -E "FAILED|Failure|Expected|Which is|error" | head -30
  return $rc
}
run_perf() { "$BUILD_DIR/hootedit-perf-gate" 2>&1 | grep -E "^\[  (PASSED|FAILED)  \]|Failure|took|slowed|scanned|copies" ; return "${PIPESTATUS[0]}"; }
run_ui() {
  if [ -z "${DISPLAY:-}" ] && ! command -v xvfb-run >/dev/null 2>&1; then echo "no display and no xvfb-run: cannot open a window"; return 2; fi
  "$ROOT/tests/ui_test.sh" "$BUILD_DIR/hootedit"
}
run_static() { "$ROOT/scripts/check-static.sh"; }

for g in "${GROUPS_WANTED[@]}"; do
  echo "==> $g"
  "run_$g"; rc=$?
  if [ $rc = 0 ]; then record "$g" PASS; elif [ $rc = 2 ] && [ "$g" = ui ]; then record "$g" SKIP; else record "$g" FAIL; fi
done
echo; echo "==> Summary"
fail=0
for i in "${!NAMES[@]}"; do printf '  %-8s %s\n' "${NAMES[$i]}" "${RESULTS[$i]}"; [ "${RESULTS[$i]}" = FAIL ] && fail=1; done
exit $fail

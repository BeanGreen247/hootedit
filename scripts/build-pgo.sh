#!/usr/bin/env bash
# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT
#
# Profile-guided build in build-pgo/: instrumented build, training run (scripts/pgo-train.sh), then the final build that uses the
# profile. Both stages share one directory on purpose: GCC looks for the profile where the instrumented objects were compiled.
#   scripts/build-pgo.sh [--prefix DIR]     build; the result is build-pgo/hootedit (install.sh installs it)
#   scripts/build-pgo.sh --keep-profile     skip the training run and reuse the profile already in build-pgo/
set -euo pipefail
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SRC"
PREFIX=/usr/local; KEEP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="$2"; shift ;;
    --keep-profile) KEEP=1 ;;
    -h|--help) sed -n '5,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done
B=build-pgo
# keep meson/ninja quiet, but print their output when they fail (meson writes its errors to stdout)
quiet() { local out; out=$("$@" 2>&1) || { printf '%s\n' "$out" >&2; return 1; }; }
if [ "$KEEP" = 0 ]; then rm -rf "$B"; fi
if [ ! -d "$B" ]; then
  quiet meson setup "$B" --prefix "$PREFIX" --buildtype release -Db_pgo=generate -Dcpp_args="-fprofile-update=prefer-atomic"
else
  quiet meson configure "$B" --prefix "$PREFIX"
fi
if [ "$KEEP" = 0 ]; then
  echo "  pgo 1/3: instrumented build"
  quiet ninja -C "$B" hootedit
  echo "  pgo 2/3: training run"
  "$SRC/scripts/pgo-train.sh" "$B/hootedit"
fi
find "$B" -name '*.gcda' | grep -q . || { echo "error: no profile data was written; the training run did not complete." >&2; exit 1; }
echo "  pgo 3/3: optimised build"
quiet meson configure "$B" -Db_pgo=use -Dcpp_args="-fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch"
quiet ninja -C "$B" hootedit
echo "  built $B/hootedit"

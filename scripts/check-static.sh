#!/usr/bin/env bash
# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT
# Static checks that need no build: every shell script parses, every relative link in README.md and docs/*.md points at a file that exists,
# every source file has the license header. Exit status 0 when all pass.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
bad=0
for f in install.sh uninstall.sh scripts/*.sh tests/*.sh; do bash -n "$f" || { echo "syntax error in $f"; bad=1; }; done
for md in README.md docs/*.md; do
  dir=$(dirname "$md")
  while read -r link; do
    [ -e "$dir/$link" ] || { echo "$md: broken link $link"; bad=1; }
  done < <(grep -oE '\]\(([^)#]+)(#[^)]*)?\)' "$md" | sed -E 's/^\]\(([^)#]+).*$/\1/' | grep -vE '^(https?:|mailto:)' || true)
done
for src in src/*.cpp src/*.h tests/*.cpp tools/*.c; do
  head -5 "$src" | grep -q "SPDX-License-Identifier\|Copyright" || head -3 "$src" | grep -q "^/\*" || { echo "no header in $src"; bad=1; }
done
[ "$bad" = 0 ] && echo "static checks passed"
exit $bad

#!/usr/bin/env bash
# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT
#
# Training run for the profile-guided build: drives an instrumented hootedit through the paths people use most (open and scroll a big
# file, type, undo, find, replace, vim commands, tabs, palette, quick open, sidebar views, git view, terminal) and runs the headless
# CPU benchmark (lexer, regex scans, fuzzy, edits, workspace search). GCC writes the profile when each process exits normally,
# so every run below ends on its own (--shot); nothing is killed.
#   scripts/pgo-train.sh PATH/TO/instrumented/hootedit
set -uo pipefail
BIN="$(readlink -f "${1:?usage: pgo-train.sh PATH/TO/instrumented/hootedit}")"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if command -v xvfb-run >/dev/null 2>&1; then
  if [ -z "${HOOTEDIT_PGO_INNER:-}" ]; then HOOTEDIT_PGO_INNER=1 exec xvfb-run -a -s "-screen 0 1280x1024x24" "$0" "$@"; fi
elif [ -z "${DISPLAY:-}" ]; then
  echo "pgo-train: no display and no xvfb-run" >&2; exit 3
fi
T="$(mktemp -d /tmp/hootedit_pgo_XXXXXX)"
trap 'rm -rf "$T"' EXIT
export XDG_CONFIG_HOME="$T/cfg"

# training data: the project's own sources, repeated into one big C++ file, plus a small text and a markdown file
: > "$T/big.cpp"
for i in 1 2 3 4 5 6 7 8; do cat "$SRC"/src/*.cpp "$SRC"/src/*.h >> "$T/big.cpp"; done
cp "$SRC/README.md" "$T/doc.md"
mkdir -p "$T/ws/src"
cp "$SRC"/src/*.cpp "$SRC"/src/*.h "$T/ws/src/" 2>/dev/null
cp "$SRC/README.md" "$T/ws/"
( cd "$T/ws" && git init -q . && git config user.email t@t && git config user.name t && git add -A && git commit -qm init \
  && echo "// change" >> src/vim.cpp && rm -f src/benchcore.h && echo new > new.txt && echo "x" >> README.md ) >/dev/null 2>&1

ok=0; bad=0
run() {   # run FILE|DIR... -- "script"
  local files=() script
  while [ "$1" != "--" ]; do files+=("$1"); shift; done
  shift; script="$1"
  if (cd "$T/ws" && timeout 90 "$BIN" --size 1024x768 "${files[@]}" --script "$script" --shot "$T/shot.ppm" >/dev/null 2>&1); then ok=$((ok + 1)); else bad=$((bad + 1)); fi
}

PGDN="pagedown;pagedown;pagedown;pagedown;pagedown;pagedown;pagedown;pagedown"
# 1. open, scroll, type, undo, find, goto, multi-cursor-like edits, comments, indentation, line moves
run "$T/big.cpp" -- "wait:300;$PGDN;ctrl+end;ctrl+home;down;down;down;up;text:hello world;ctrl+z;ctrl+z;ctrl+f;text:static;return;return;return;f3;escape;ctrl+g;text:900;return;ctrl+d;ctrl+d;ctrl+/;ctrl+];alt+down;alt+up;ctrl+shift+k;ctrl+l;text:x;ctrl+z;$PGDN;ctrl+b;ctrl+b;wait:100"
# 2. replace, tabs, quick open, palette, themes, outline
run "$T/big.cpp" "$T/doc.md" -- "wait:200;ctrl+h;text:int;tab;text:long;return;escape;ctrl+tab;ctrl+tab;act:outline;act:view_explorer;wait:100;mouse:5,5;click:120,130;mouse:5,5;ctrl+p;text:vim;down;return;ctrl+shift+p;text:theme;return;escape;ctrl+w;ctrl+k;m;wait:100"
# 3. vim: motions, operators, objects, search, :s, :g, macros, registers, visual and block modes
run "$T/big.cpp" -- "act:vim;wait:200;text:gg5jddu;text:wciwfoo;escape;text:.;text:/;text:static;return;text:nnN*#;text::;text:%s/int/long/g;return;text:u;text::;text:g/static/s/static/STATIC/;return;text:u;text:qaA;text:;;escape;text:jq;text:40@a;text:v3jd;text:p;text:V4jy;text:P;ctrl+v;text:3j4ly;text:p;text:yiwwdawu;text:ci(x;escape;text:ysiw);text:cs);text:ds);text:5Gmaggd'a;text:xp~~;text:rx;text:J;text:>>;text:<<;text:gUiw;text:guu;ctrl+a;ctrl+x;text:Rabc;escape;text:zzztzb;text:HML;text:%;text:{};text:fxtxFxTx;text:;,;text:gg;text:G;text:u;text:u;ctrl+r;wait:100"
# 4. source control, search tab, remote tab, diff tabs (the workspace is a git repository with changes)
run "$T/ws" -- "act:view_git;wait:700;mouse:5,5;click:100,200;mouse:5,5;click:100,230;mouse:5,5;wait:300;act:search;text:static;wait:1500;mouse:5,5;click:100,200;wait:200;act:view_explorer;wait:100;act:view_git;wait:100"
# 5. terminal panel
run "$T/ws" -- "act:term_toggle;wait:400;text:ls;return;wait:300;text:echo hello;return;wait:300;act:term_toggle;wait:100"
# 6. headless CPU paths
"$BIN" --bench-core "$T/big.cpp" 2 "$SRC" >/dev/null 2>&1 && ok=$((ok + 1)) || bad=$((bad + 1))
[ -d /usr/include ] && { "$BIN" --bench-core "$T/doc.md" 1 /usr/include >/dev/null 2>&1 && ok=$((ok + 1)) || bad=$((bad + 1)); }
echo "pgo-train: $ok runs ok, $bad failed"
[ "$ok" -ge 5 ]

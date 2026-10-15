#!/usr/bin/env bash
# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT
#
# UI scenarios: the real editor window driven by --script, state read back with the "dump" step (no pixel compares).
# Runs under xvfb-run when there is no display; exits 77 (skipped) when neither is available.
# usage: ui_test.sh PATH/TO/hootedit
set -uo pipefail
BIN="${1:?usage: ui_test.sh PATH/TO/hootedit}"
BIN="$(readlink -f "$BIN")"
if [ -z "${DISPLAY:-}" ]; then
  if command -v xvfb-run >/dev/null 2>&1; then exec xvfb-run -a -s "-screen 0 1280x1024x24" "$0" "$@"; fi
  echo "no display and no xvfb-run: skipped"; exit 77
fi
T="$(mktemp -d /tmp/hootedit_ui_XXXXXX)"
trap 'rm -rf "$T"' EXIT
export XDG_CONFIG_HOME="$T/cfg"
fails=0; total=0

# run "script steps" [files...]  -> output in $OUT
run() {
  local steps="$1"; shift
  rm -rf "$T/cfg"
  OUT="$(cd "$T/ws" && timeout 60 "$BIN" --size 1024x768 "$@" --script "$steps" --shot "$T/shot.ppm" 2>&1)"
}
ok() {   # ok "name" "exact line prefix expected"
  total=$((total + 1))
  if printf '%s\n' "$OUT" | grep -qxF -- "$2"; then return 0; fi
  fails=$((fails + 1)); echo "FAIL: $1"; echo "   wanted line: $2"; printf '%s\n' "$OUT" | head -30 | sed 's/^/   | /'
}
has() {  # has "name" "substring"
  total=$((total + 1))
  if printf '%s\n' "$OUT" | grep -qF -- "$2"; then return 0; fi
  fails=$((fails + 1)); echo "FAIL: $1"; echo "   wanted substring: $2"; printf '%s\n' "$OUT" | head -30 | sed 's/^/   | /'
}
hasnot() {
  total=$((total + 1))
  if ! printf '%s\n' "$OUT" | grep -qF -- "$2"; then return 0; fi
  fails=$((fails + 1)); echo "FAIL: $1"; echo "   unexpected substring: $2"
}

mkdir -p "$T/ws/src/lib" "$T/ws/plain"
printf 'one two three\nfour five\nsix\n' > "$T/ws/a.txt"
printf '#include <x.h>\n\nstruct P {\n    int x;\n};\n\nstatic int add(int a, int b) {\n    return a + b;\n}\n' > "$T/ws/t.c"
printf 'x\n' > "$T/ws/src/lib/deep.txt"

# ---- typing and saving ----
run "text:hello;ctrl+s;wait:200;dump" a.txt
has "typing goes into the buffer" "text: helloone two three\\nfour five\\nsix\\n"
ok "ctrl+s saves" "status: saved a.txt"
total=$((total + 1)); [ "$(head -c 5 "$T/ws/a.txt")" = "hello" ] || { fails=$((fails + 1)); echo "FAIL: file on disk not written"; }
printf 'one two three\nfour five\nsix\n' > "$T/ws/a.txt"

# ---- vim mode ----
run "act:vim;text:dd;dump" a.txt
has "vim dd deletes a line" "text: four five\\nsix\\n"
has "vim shows normal mode" "vim: -- NORMAL --"
run "act:vim;text:wcwXX;escape;text:w.;dump" a.txt
has "vim cw and dot repeat" "text: one XX XX\\nfour five\\nsix\\n"
run "act:vim;text:ddjp;dump" a.txt
has "vim dd then p" "text: four five\\nsix\\none two three\\n"
run "act:vim;text:ddu;dump" a.txt
has "vim undo" "text: one two three\\nfour five\\nsix\\n"
run "act:vim;text:vjd;dump" a.txt
has "vim visual delete" "text: our five\\nsix\\n"
run "act:vim;text:qaA!;escape;text:jq;text:@a;dump" a.txt
has "vim macro" "text: one two three!\\nfour five!\\nsix\\n"
run "act:vim;text:ci;text:w;text:Z;escape;dump" a.txt
has "vim ciw" "text: Z two three"
run "act:vim;text::;text:%s/o/0/g;return;dump" a.txt
has "vim :%s" "text: 0ne tw0 three\\nf0ur five\\nsix\\n"
run "act:vim;text:/;text:five;return;dump" a.txt
ok "vim / search moves the cursor" "cursor: 2,6"
run "act:vim;text:G;text:/;text:one;return;dump" a.txt
ok "vim search wraps" "cursor: 1,1"
run "act:vim;text:ihi;escape;text::;text:wq;return;dump" a.txt
total=$((total + 1)); [ "$(head -c 2 "$T/ws/a.txt")" = "hi" ] || { fails=$((fails + 1)); echo "FAIL: :wq did not write the file"; }
printf 'one two three\nfour five\nsix\n' > "$T/ws/a.txt"
run "act:vim;text:G;text:?;text:four;return;dump" a.txt
ok "vim ? searches backward" "cursor: 2,1"
run "act:vim;text:j;text:/;text:o;return;text:n;text:N;dump" a.txt
ok "vim n then N goes back" "cursor: 2,2"
run "act:vim;text::;text:%s/o/0/g;return;text:u;dump" a.txt
has "vim :%s is one undo step" "text: one two three\\nfour five\\nsix\\n"
run "act:vim;text:ysiw);dump" a.txt
has "vim surround" "text: (one) two three"
run "act:vim;text:ix;escape;act:vim;text:ix;dump" a.txt
has "vim off: keys type text" "vim: off"

run "act:vim;text:l;ctrl+v;text:jld;dump" a.txt
has "vim block delete" "text: o two three\nfr five\nsix\n"
run "act:vim;text:RXY;escape;dump" a.txt
has "vim replace mode" "text: XYe two three\nfour five\nsix\n"
run "act:vim;text::;text:g/o/d;return;dump" a.txt
has "vim :g/pat/d" "text: six\n"
run "act:vim;text::;text:g/o/s/o/0/g;return;dump" a.txt
has "vim :g with :s" "text: 0ne tw0 three\nf0ur five\nsix\n"
run "act:vim;text::;text:v/o/d;return;dump" a.txt
has "vim :v/pat/d" "text: one two three\nfour five\n"
run "act:vim;text::;text:%normal Ax;return;dump" a.txt
has "vim :normal over a range" "text: one two threex\nfour fivex\nsixx\n"

# ---- outline ----
run "act:outline;act:view_explorer;dump" t.c
ok "outline lists the type" "outline: P line=3 depth=0 kind=C"
ok "outline lists the function" "outline: add line=7 depth=0 kind=F"
run "act:view_explorer;dump" t.c
hasnot "outline is off by default" "outline:"

# ---- explorer: preview tabs, compact folders ----
run "act:view_explorer;wait:300;mouse:5,5;dump" t.c
has "compact folder row" "row: src/lib depth=0 dir=1"
run "act:view_explorer;wait:300;mouse:5,5;click:80,222;mouse:5,5;dump" t.c
ok "a single click opens one more tab" "tabs: 2"
has "the new tab is a preview" "preview=1"

# ---- git status in the Explorer ----
( cd "$T/ws" && git init -q . && git config user.email t@t && git config user.name t && git add -A && git commit -qm init \
  && echo more >> a.txt && rm t.c && echo n > new.txt ) >/dev/null 2>&1
run "act:view_explorer;wait:600;mouse:5,5;dump" a.txt
has "modified file is marked" "row: a.txt depth=0 dir=0 ghost=0 deco=M"
has "deleted file stays as a ghost row" "row: t.c depth=0 dir=0 ghost=1 deco=D"
has "untracked file is marked" "row: new.txt depth=0 dir=0 ghost=0 deco=?"

# ---- shortcuts list and quick open ----
run "act:shortcuts;dump" a.txt
ok "shortcuts list opens the picker" "mode: 2"
run "ctrl+p;text:new;down;dump" a.txt
ok "quick open is the quick mode" "mode: 6"
run "ctrl+p;text:new;return;dump" a.txt
has "quick open Enter opens the file" "tab 1: new.txt preview=0"

echo "ui scenarios: $((total - fails))/$total passed"
[ "$fails" = 0 ]

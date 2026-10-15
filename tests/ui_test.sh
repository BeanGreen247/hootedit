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
has "ctrl+s saves" "/ws/a.txt"
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

# ---- live checks: Ansible rules, fixes, next problem, noqa, toggle ----
mkdir -p "$T/ws/role/tasks"
printf -- '- name: A\n  file:\n    path: /x\n    mode: 644\n- name: B\n  shell: ls  # noqa\n' > "$T/ws/role/tasks/main.yml"
run "dump" role/tasks/main.yml
has "ansible fqcn rule" "diag: 2:3 warning fqcn[action-core]"
has "octal rule" "diag: 4:11 warning risky-octal"
hasnot "noqa silences the line" "diag: 6:"
run "act:fixall;dump" role/tasks/main.yml
has "fix all rewrites the module name" "text: - name: A\\n  ansible.builtin.file:\\n    path: /x\\n    mode: \"0644\"\\n"
run "act:next_problem;act:quickfix;dump" role/tasks/main.yml
has "quick fix applies the fix at the cursor" "ansible.builtin.file:"
run "act:next_problem;dump" role/tasks/main.yml
ok "next problem moves the cursor to the first one" "cursor: 2,3"
run "act:lint;dump" role/tasks/main.yml
hasnot "live checks can be turned off" "diag:"
printf 'int main(void) {\n    char b[4];\n    gets(b);\n    if (b[0] = 1) return 0;\n' > "$T/ws/bad.c"
run "dump" bad.c
has "C: gets" "diag: 3:5 error c-gets"
has "C: assignment in condition" "c-assign-in-condition"
has "C: unclosed brace" "bracket Unclosed '{'"
printf '#!/bin/bash\necho a\ncd /tmp\nrm -rf $DIR/x\necho b\necho c\n' > "$T/ws/bad.sh"
run "dump" bad.sh
has "shell: rm -rf with a variable" "rm-rf-variable"
has "shell: no set -e" "shell-no-set-e"
printf '{"a": [1, 2,]}\n' > "$T/ws/bad.json"
run "dump" bad.json
has "JSON syntax error" "json-syntax"

# ---- Kubernetes ----
printf 'apiVersion: extensions/v1beta1\nkind: Ingress\nmetadata:\n  name: Web\n  labels:\n    tier: 1\nspec:\n  rules: []\n' > "$T/ws/ing.yaml"
run "dump" ing.yaml
has "k8s: removed apiVersion" "diag: 1:"
has "k8s: removed apiVersion code" "k8s-api-removed"
has "k8s: name rule" "k8s-name"
has "k8s: label value must be a string" "k8s-string-value"
run "act:fixall;dump" ing.yaml
has "k8s: fix all corrects the apiVersion" "text: apiVersion: networking.k8s.io/v1\\nkind: Ingress"
has "k8s: fix all quotes the label" "tier: \"1\""
: > "$T/ws/new.yaml"
run "act:kubernetes;return;dump" new.yaml
has "k8s: insert resource puts a Deployment into an empty file" "text: apiVersion: apps/v1\\nkind: Deployment"
hasnot "k8s: the inserted Deployment is clean" "diag:"
printf '[a]\nx = 1\nx = 2\n' > "$T/ws/dup.toml"
run "dump" dup.toml
has "TOML duplicate key" "toml-duplicate-key"
printf '<a>\n<b>\n</a>\n' > "$T/ws/bad.xml"
run "dump" bad.xml
has "XML mismatch" "xml-mismatch"
printf 'DELETE FROM users;\n' > "$T/ws/bad.sql"
run "dump" bad.sql
has "SQL delete without where" "sql-delete-all"

# ---- hot reload: a file changed by another program ----
printf 'one\ntwo\n' > "$T/ws/hot.txt"
( sleep 1; printf 'one\nchanged outside\nthree\n' > "$T/ws/hot.txt" ) &
run "wait:~300;wait:~1800;dump" hot.txt
has "an unmodified file reloads by itself" "text: one\\nchanged outside\\nthree\\n"
has "the reload says how long it took" "reloaded hot.txt from disk in"
printf 'one\ntwo\n' > "$T/ws/hot.txt"
( sleep 1; printf 'rewritten\n' > "$T/ws/hot.txt" ) &
run "wait:~300;text:mine;wait:~1800;dump" hot.txt
ok "unsaved edits are not overwritten: a dialog asks" "mode: 10"
has "the editor text is still the user's" "text: mineone\\ntwo\\n"
( sleep 1; echo new > "$T/ws/zz_created_later.txt" ) &
run "wait:~300;wait:~1800;dump" hot.txt
has "a file created by another program shows in the Explorer" "row: zz_created_later.txt"
rm -f "$T/ws/zz_created_later.txt"

# ---- custom resources are checked against the CRD in the workspace ----
printf 'apiVersion: apiextensions.k8s.io/v1\nkind: CustomResourceDefinition\nmetadata:\n  name: widgets.example.com\nspec:\n  group: example.com\n  names:\n    kind: Widget\n    plural: widgets\n  scope: Namespaced\n  versions:\n    - name: v1\n      served: true\n      storage: true\n      schema:\n        openAPIV3Schema:\n          type: object\n          properties:\n            spec:\n              type: object\n              properties:\n                size:\n                  type: integer\n' > "$T/ws/widget-crd.yaml"
printf 'apiVersion: example.com/v1\nkind: Widget\nmetadata:\n  name: w\nspec:\n  sise: 3\n' > "$T/ws/widget.yaml"
run "dump;wait:~1500;dump" widget.yaml
has "a custom resource is checked against its CRD after the scan" "k8s-unknown-field Unknown field 'sise'"
printf 'apiVersion: apps/v1\nkind: Deployment\nmetadata:\n  name: x\nspec:\n  replica: 2\n' > "$T/ws/typo.yaml"
run "dump" typo.yaml
has "a misspelt field name is caught from the API tables" "Did you mean 'replicas'?"

# ---- Settings page ----
run "act:settings;text:vim;dump" typo.yaml
has "the Settings page opens and its search narrows the list" "settings: open=1 category=0 query=vim rows=2"
run "act:settings;down;return;dump" typo.yaml
has "Enter on a setting toggles it and the page stays" "settings: open=1"
run "ctrl+,;dump" typo.yaml
has "Ctrl+, opens the Settings page" "settings: open=1"

run "wait:200;mouse:5,5;click:14,725;mouse:5,5;dump" typo.yaml
has "the cog at the foot of the sidebar rail opens Settings" "settings: open=1"

echo "ui scenarios: $((total - fails))/$total passed"
[ "$fails" = 0 ]

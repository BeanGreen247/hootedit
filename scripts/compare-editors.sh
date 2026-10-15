#!/usr/bin/env bash
# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT
#
# Same-machine comparison of graphical editors: time until the window is painted, memory (PSS of the whole process tree), idle CPU,
# and key-to-pixel typing latency (tools/xlatency.c, the Linux counterpart of Typometer / Keypress.sh), on an empty file and on a big one.
# Every editor runs on its own fresh Xvfb display with a throwaway profile, window 1024x768 at 0,0, so numbers are relative to each other
# and to this machine (software rendering, no compositor, no monitor scan-out). Editors that are not installed are skipped.
#
#   "code-yours" is your own VS Code: your settings.json, keybindings, snippets and every installed extension, copied into a throwaway profile
#   (nothing in your real profile is changed; Settings Sync data and login state are not copied). "code" is VS Code with no extensions and default settings.
#   scripts/compare-editors.sh [--editors "hootedit code code-yours subl zed kate gedit"] [--big FILE] [--samples N] [--hootedit PATH]
#   output: a markdown table on stdout
set -uo pipefail
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EDITORS="hootedit code code-yours codium subl zeditor zed kate gedit geany lite-xl"
BIG=""; SAMPLES=100; HBIN="$SRC/build/hootedit"
while [ $# -gt 0 ]; do
  case "$1" in
    --editors) EDITORS="$2"; shift ;;
    --big) BIG="$2"; shift ;;
    --samples) SAMPLES="$2"; shift ;;
    --hootedit) HBIN="$2"; shift ;;
    -h|--help) sed -n '5,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done
for t in Xvfb xdotool xwininfo gcc python3; do command -v $t >/dev/null || { echo "need $t" >&2; exit 1; }; done
T="$(mktemp -d /tmp/hootedit_cmp_XXXXXX)"
XPIDS=()
cleanup() { for p in "${XPIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; rm -rf "$T"; }
trap cleanup EXIT
gcc -O2 "$SRC/tools/xlatency.c" -o "$T/xlatency" $(pkg-config --cflags --libs x11 xtst xext) || exit 1
: > "$T/empty.txt"
mkdir -p "$T/ans/roles/web/tasks"
PLAY="$T/ans/roles/web/tasks/main.yml"   # a realistic role: named and unnamed tasks, short module names, quoted and unquoted modes
{
  echo "---"
  for i in $(seq 1 100); do
    printf -- '- name: Install package %s\n  apt:\n    name: pkg%s\n    state: latest\n\n' "$i" "$i"
    printf -- '- name: Copy file %s\n  ansible.builtin.copy:\n    src: files/f%s\n    dest: /etc/app/f%s\n    mode: 0644\n    owner: root\n  notify: Restart app\n\n' "$i" "$i" "$i"
    printf -- '- name: run step %s\n  shell: /opt/app/step%s.sh\n  register: out%s\n  when: "{{ enable_%s }}"\n\n' "$i" "$i" "$i" "$i"
  done
} > "$PLAY"
if [ -z "$BIG" ]; then
  BIG="$T/big.c"; : > "$BIG"
  (find /usr/include -name '*.h' -size +2k 2>/dev/null | xargs cat 2>/dev/null) | head -c 40000000 > "$BIG"
fi
BIGMB=$(( $(stat -c %s "$BIG") / 1000000 ))

tree_pids() { local p=$1 c; echo "$p"; for c in $(pgrep -P "$p" 2>/dev/null); do tree_pids "$c"; done; }
pss_mb() { local sum=0 p v; for p in $(tree_pids "$1"); do v=$(awk '/^Pss:/ {s+=$2} END {print s+0}' "/proc/$p/smaps_rollup" 2>/dev/null); sum=$((sum + ${v:-0})); done; echo $((sum / 1024)); }
cpu_ticks() { local sum=0 p v; for p in $(tree_pids "$1"); do v=$(awk '{print $14+$15}' "/proc/$p/stat" 2>/dev/null); sum=$((sum + ${v:-0})); done; echo "$sum"; }
launch_cmd() {   # launch_cmd EDITOR FILE
  case "$1" in
    hootedit) echo "env XDG_CONFIG_HOME=$T/cfg $HBIN --size 1024x768 $2" ;;
    code|codium) echo "$1 --user-data-dir=$T/ud --extensions-dir=$T/ext --disable-extensions --disable-gpu --disable-workspace-trust --no-sandbox --new-window $2" ;;
    code-yours) echo "code --user-data-dir=$T/ud --extensions-dir=$T/ext --disable-gpu --disable-workspace-trust --no-sandbox --new-window $2" ;;
    zed|zeditor) echo "$1 --user-data-dir $T/zed $2" ;;
    *) echo "$1 $2" ;;
  esac
}
have() {
  [ "$1" = hootedit ] && { [ -x "$HBIN" ]; return; }
  [ "$1" = code-yours ] && { command -v code >/dev/null && [ -f "$HOME/.config/Code/User/settings.json" ]; return; }
  command -v "$1" >/dev/null
}

measure() {   # measure EDITOR FILE LABEL
  local ed="$1" file="$2" label="$3" disp=":$((90 + RANDOM % 50))" xpid wid wpid ready p0 p1 idle pss lat t0
  Xvfb "$disp" -screen 0 1280x1024x24 >/dev/null 2>&1 & xpid=$!; XPIDS+=("$xpid"); sleep 1.2
  export DISPLAY="$disp"
  local mpid=""
  if command -v marco >/dev/null; then marco --display="$disp" --sm-disable >/dev/null 2>&1 & mpid=$!; sleep 1; fi   # some editors (Electron) ignore keys until a window manager marks their window active
  rm -rf "$T/cfg" "$T/ud" "$T/ext" "$T/zed"
  if [ "$ed" = code ]; then mkdir -p "$T/ud/User"; echo '{}' > "$T/ud/User/settings.json"; fi
  if [ "$ed" = code-yours ]; then   # a copy of your settings, keybindings, snippets and extensions; the real profile is never opened
    mkdir -p "$T/ud/User"
    cp "$HOME/.config/Code/User/settings.json" "$T/ud/User/" 2>/dev/null
    cp "$HOME/.config/Code/User/keybindings.json" "$T/ud/User/" 2>/dev/null
    [ -d "$HOME/.config/Code/User/snippets" ] && cp -r "$HOME/.config/Code/User/snippets" "$T/ud/User/"
    [ -d "$HOME/.vscode/extensions" ] && cp -r "$HOME/.vscode/extensions" "$T/ext"
  fi
  if [ "$ed" = code ] || [ "$ed" = code-yours ]; then   # only things that stop a first-run dialog from covering the editor; nothing that changes speed
    python3 - "$T/ud/User/settings.json" <<'PY'
import re, sys
p = sys.argv[1]
t = open(p).read() if __import__("os").path.exists(p) else "{}"
extra = '"chat.disableAIFeatures": true, "update.mode": "none", "telemetry.telemetryLevel": "off", "extensions.autoUpdate": false, "workbench.welcomePage.walkthroughs.openOnInstall": false'
i = t.rstrip().rfind("}")
body = t[:i].rstrip()
sep = "" if body.endswith("{") or body.endswith(",") else ","
open(p, "w").write(body + sep + "\n  " + extra + "\n}\n")
PY
  fi
  t0=$(date +%s%3N)
  bash -c "exec $(launch_cmd "$ed" "$file")" >/dev/null 2>&1 &
  local launched=$!
  wid=""
  for _ in $(seq 1 600); do   # the largest viewable window of at least 300x200
    wid=$(xwininfo -root -tree 2>/dev/null | awk '/^ +0x/ && /[0-9]+x[0-9]+\+/ { split($0,a," "); if (match($0,/([0-9]+)x([0-9]+)\+/,m) && m[1]>=300 && m[2]>=200) { print a[1]; exit } }')
    [ -n "$wid" ] && break
    sleep 0.05
  done
  if [ -z "$wid" ]; then echo "| $ed | $label | no window | | | | |"; kill "$xpid" 2>/dev/null; return; fi
  xdotool windowmove "$wid" 0 0 windowsize "$wid" 1024 768 2>/dev/null; sleep 0.5
  local ax ay rect   # where the window really is (a window manager may keep its own position): watch the top 240 px of it
  read -r ax ay <<<"$(xwininfo -id "$wid" 2>/dev/null | awk '/Absolute upper-left X/ {x=$4} /Absolute upper-left Y/ {y=$4} END {print x+0, y+0}')"
  rect="$ax,$ay,$(( 1280 - ax > 1024 ? 1024 : 1280 - ax )),240"
  ready=$("$T/xlatency" -P -s "$t0" -r "$rect" -T 90000 2>/dev/null | sed 's/ready=//')
  wpid=$(xdotool getwindowpid "$wid" 2>/dev/null || echo "$launched")
  sleep 10   # settle: start-up work, and Hootedit stops blinking its caret after 10 s idle
  pss=$(pss_mb "$wpid")
  p0=$(cpu_ticks "$wpid"); sleep 5; p1=$(cpu_ticks "$wpid")
  idle=$(awk -v a="$p0" -v b="$p1" -v hz="$(getconf CLK_TCK)" 'BEGIN {printf "%.1f", (b - a) / hz / 5 * 100}')
  xdotool windowactivate "$wid" 2>/dev/null; sleep 0.3; xdotool mousemove $((ax + 600)) $((ay + 400)) click 1; sleep 0.4; xdotool key ctrl+Home; sleep 0.4   # caret to the first line, inside the watched rectangle
  lat=$("$T/xlatency" -n "$SAMPLES" -d 100 -w 5 -r "$rect" 2>&1)
  echo "| $ed | $label | $ready | ${pss} MB | ${idle}% | $lat |"
  for p in $(tree_pids "$wpid" | tac); do kill "$p" 2>/dev/null; done
  kill "$launched" 2>/dev/null
  for _ in $(seq 1 40); do kill -0 "$wpid" 2>/dev/null || break; sleep 0.05; done   # let the editor exit before its display goes away
  [ -n "$mpid" ] && kill "$mpid" 2>/dev/null
  kill "$xpid" 2>/dev/null; sleep 0.3
}

echo "Machine: $(uname -sr), $(nproc) cores; display: Xvfb software rendering, window 1024x768; samples: $SAMPLES at 100 ms spacing"
echo
echo "| editor | file | window painted | memory (PSS) | idle CPU | typing latency (key to pixels) |"
echo "|---|---|---|---|---|---|"
for ed in $EDITORS; do
  have "$ed" || continue
  measure "$ed" "$T/empty.txt" "empty"
  measure "$ed" "$BIG" "${BIGMB} MB C"
  measure "$ed" "$PLAY" "Ansible role, 300 tasks"
done

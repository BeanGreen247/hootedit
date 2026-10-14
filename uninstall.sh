#!/usr/bin/env bash
# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT
#
# Removes Hootedit's program, desktop entry and icon. Your files and settings are never touched.
#   ./uninstall.sh                removes every copy found in /usr/local and ~/.local
#   ./uninstall.sh --system       only /usr/local
#   ./uninstall.sh --user         only ~/.local
#   ./uninstall.sh --prefix DIR   only DIR
#   --dry-run                     show what would be removed
set -euo pipefail

PREFIXES=(); DRY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --system) PREFIXES+=(/usr/local) ;;
    --user) PREFIXES+=("$HOME/.local") ;;
    --prefix) PREFIXES+=("$2"); shift ;;
    --dry-run) DRY=1 ;;
    -h|--help) sed -n '5,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
  shift
done
[ ${#PREFIXES[@]} -gt 0 ] || PREFIXES=(/usr/local "$HOME/.local")

removed=0
for p in "${PREFIXES[@]}"; do
  for f in "$p/bin/hootedit" "$p/share/applications/hootedit.desktop" "$p/share/icons/hicolor/256x256/apps/hootedit.png"; do
    [ -e "$f" ] || continue
    SUDO=(); [ -w "$(dirname "$f")" ] || SUDO=(sudo)
    if [ "$DRY" = 1 ]; then echo "[dry run] ${SUDO[*]:-} rm $f"; else "${SUDO[@]}" rm -v -- "$f"; fi
    removed=$((removed + 1))
  done
  if [ "$DRY" = 0 ] && command -v update-desktop-database >/dev/null && [ -d "$p/share/applications" ]; then
    if [ -w "$p/share/applications" ]; then update-desktop-database "$p/share/applications" 2>/dev/null || true; else sudo update-desktop-database "$p/share/applications" 2>/dev/null || true; fi
  fi
done
for p in "${PREFIXES[@]}"; do   # keep an existing icon cache honest about the icon that just went away
  ICONDIR="$p/share/icons/hicolor"
  if [ "$DRY" = 0 ] && [ "$removed" -gt 0 ] && [ -f "$ICONDIR/icon-theme.cache" ] && command -v gtk-update-icon-cache >/dev/null; then
    SUDO=(); [ -w "$ICONDIR" ] || SUDO=(sudo)
    "${SUDO[@]}" gtk-update-icon-cache -q -f -t "$ICONDIR" 2>/dev/null || true
  fi
done
[ "$removed" -gt 0 ] && echo "Hootedit was removed." || echo "Hootedit is not installed in: ${PREFIXES[*]}"

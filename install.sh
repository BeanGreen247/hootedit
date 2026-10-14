#!/usr/bin/env bash
# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT
#
# Hootedit installer: builds from source and installs system-wide (/usr/local) or for your user (~/.local).
# Run it as your normal user; it asks for the sudo password once, only when it needs it.
#   ./install.sh                  ask where (system-wide or ~/.local); non-interactive default is system-wide, like Lestrix
#   ./install.sh --system         install to /usr/local
#   ./install.sh --user           install to ~/.local (no sudo for the files)
#   ./install.sh --prefix DIR     install under DIR
#   ./install.sh --uninstall      remove it (same location flags; without one it removes every copy it finds)
#   --no-deps                     never touch system packages
#   --tests                       run the test suite before installing
#   --dry-run                     show the steps, change nothing
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SRC"

PREFIX=""; UNINSTALL=0; DEPS=yes; TESTS=0; DRY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --system) PREFIX=/usr/local ;;
    --user) PREFIX="$HOME/.local" ;;
    --prefix) PREFIX="$2"; shift ;;
    --uninstall) UNINSTALL=1 ;;
    --no-deps) DEPS=no ;;
    --tests) TESTS=1 ;;
    --dry-run) DRY=1 ;;
    -h|--help) sed -n '5,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
  shift
done

if [ "$(uname -s)" != Linux ]; then echo "error: Hootedit builds on Linux only for now. Nothing changed." >&2; exit 1; fi
if [ "${EUID}" -eq 0 ]; then
  echo "error: run ./install.sh as your normal user, not with sudo or as root." >&2
  echo "       It asks for your password itself, once, when it needs it." >&2
  exit 1
fi
run() { if [ "$DRY" = 1 ]; then echo "[dry run] $*"; else "$@"; fi; }

if [ "$UNINSTALL" = 1 ]; then
  a=(); [ -n "$PREFIX" ] && a=(--prefix "$PREFIX"); [ "$DRY" = 1 ] && a+=(--dry-run)
  exec "$SRC/uninstall.sh" "${a[@]}"
fi

if [ -z "$PREFIX" ]; then
  if [ -t 0 ] && [ -t 1 ]; then
    echo "Where should Hootedit be installed?"
    echo "  1) system-wide, /usr/local (all users, needs your sudo password once)  [default]"
    echo "  2) just for you, ~/.local (no sudo for the files)"
    read -r -p "Choose 1 or 2: " ans
    case "${ans:-1}" in 2) PREFIX="$HOME/.local" ;; *) PREFIX=/usr/local ;; esac
  else
    PREFIX=/usr/local
  fi
fi
echo "Installing to $PREFIX"

# sudo is needed for packages, or when the prefix is not writable
probe="$PREFIX"; while [ ! -e "$probe" ] && [ "$probe" != / ]; do probe=$(dirname "$probe"); done
need_deps() { command -v meson >/dev/null && command -v ninja >/dev/null && command -v g++ >/dev/null && command -v pkg-config >/dev/null \
  && pkg-config --exists sdl2 freetype2 fontconfig; }
NEED_SUDO=0
[ ! -w "$probe" ] && NEED_SUDO=1
[ "$DEPS" = yes ] && ! need_deps && NEED_SUDO=1
if [ "$NEED_SUDO" = 1 ] && [ "$DRY" = 1 ]; then
  echo "[1/4] [dry run] would ask for your sudo password once"
elif [ "$NEED_SUDO" = 1 ]; then
  command -v sudo >/dev/null 2>&1 || { echo "error: sudo is not installed; use --user, or install the packages yourself with --no-deps." >&2; exit 1; }
  echo "[1/4] asking for permission once (sudo)"
  sudo -v || { echo "error: sudo permission is required for this choice; use --user to avoid it." >&2; exit 1; }
else
  echo "[1/4] no sudo needed"
fi

echo "[2/4] build requirements"
if [ "$DEPS" = no ]; then echo "  skipped (--no-deps)"
elif need_deps && [ "$DRY" = 0 ]; then echo "  everything needed is already installed"
else
  if command -v apt-get >/dev/null; then
    run sudo apt-get update -qq
    run sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y meson ninja-build g++ pkg-config libsdl2-dev libfreetype-dev libfontconfig-dev libgl-dev fontconfig fonts-dejavu-core
  elif command -v dnf >/dev/null; then run sudo dnf install -y meson ninja-build gcc-c++ pkgconf-pkg-config SDL2-devel freetype-devel fontconfig-devel mesa-libGL-devel dejavu-sans-mono-fonts
  elif command -v pacman >/dev/null; then run sudo pacman -S --needed --noconfirm meson ninja gcc pkgconf sdl2 freetype2 fontconfig mesa ttf-dejavu
  elif command -v zypper >/dev/null; then run sudo zypper --non-interactive install meson ninja gcc-c++ pkg-config libSDL2-devel freetype2-devel fontconfig-devel Mesa-libGL-devel dejavu-fonts
  else echo "warning: no supported package manager; install meson, ninja, g++, pkg-config and the SDL2, FreeType and fontconfig development files yourself." >&2; fi
  [ "$DRY" = 1 ] || need_deps || { echo "error: build requirements are still missing (see above)." >&2; exit 1; }
fi

echo "[3/4] build"
if [ "$DRY" = 1 ]; then
  echo "[dry run] meson setup build --prefix $PREFIX --buildtype release && ninja -C build"
else
  meson setup build --prefix "$PREFIX" --buildtype release --reconfigure >/dev/null 2>&1 || meson setup build --prefix "$PREFIX" --buildtype release >/dev/null
  ninja -C build
  if [ "$TESTS" = 1 ]; then meson test -C build; fi
fi

echo "[4/4] install to $PREFIX"
SUDO=(); [ ! -w "$probe" ] && SUDO=(sudo)
run "${SUDO[@]}" meson install -C build --no-rebuild
run "${SUDO[@]}" install -D -m 644 data/hootedit.desktop "$PREFIX/share/applications/hootedit.desktop"
command -v update-desktop-database >/dev/null && run "${SUDO[@]}" update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
# A stale icon-theme.cache (left by an earlier program in this prefix; -t because the prefix has no index.theme of its own) hides new icons from the menu and panel: GTK trusts it while it is newer than the folder.
ICONDIR="$PREFIX/share/icons/hicolor"
if [ -f "$ICONDIR/icon-theme.cache" ] && command -v gtk-update-icon-cache >/dev/null; then
  run "${SUDO[@]}" gtk-update-icon-cache -q -f -t "$ICONDIR" || true
fi
echo "installed: $PREFIX/bin/hootedit"
echo "remove it any time with: ./install.sh --uninstall   (or ./uninstall.sh)"
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) echo "note: add $PREFIX/bin to your PATH to run 'hootedit' from a shell." ;; esac

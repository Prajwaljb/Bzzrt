#!/bin/sh
# Builds a Release binary and assembles dist/linux/.
#   ./package-linux.sh            build + package
#   ./package-linux.sh --install  also install to ~/.local (app menu entry, icon, launcher)
set -eu
cd "$(dirname "$0")"
ROOT=$(pwd)

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

DIST="$ROOT/dist/linux"
rm -rf "$DIST"
mkdir -p "$DIST"
cp build/vrrdiag "$DIST/"
strip "$DIST/vrrdiag" 2>/dev/null || true
cp README.md docs/VRR_LFC_GUIDE.md packaging/QUICKSTART.txt packaging/linux/vrrdiag.svg "$DIST/"
# Portable launcher: runs the binary sitting next to it.
sed 's|"@VRRDIAG_BIN@"|"$(dirname "$0")/vrrdiag"|' packaging/linux/vrrdiag-launch.in > "$DIST/vrrdiag-launch"
chmod +x "$DIST/vrrdiag" "$DIST/vrrdiag-launch"
echo "Packaged $DIST"

if [ "${1:-}" = "--install" ]; then
  BIN="$HOME/.local/bin"
  APPS="$HOME/.local/share/applications"
  ICONS="$HOME/.local/share/icons/hicolor/scalable/apps"
  mkdir -p "$BIN" "$APPS" "$ICONS"
  install -m 755 "$DIST/vrrdiag" "$BIN/vrrdiag"
  sed "s|@VRRDIAG_BIN@|$BIN/vrrdiag|" packaging/linux/vrrdiag-launch.in > "$BIN/vrrdiag-launch"
  chmod 755 "$BIN/vrrdiag-launch"
  install -m 644 packaging/linux/vrrdiag.svg "$ICONS/vrrdiag.svg"
  sed "s|@VRRDIAG_LAUNCHER@|$BIN/vrrdiag-launch|" packaging/linux/vrrdiag.desktop.in > "$APPS/vrrdiag.desktop"
  command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" || true
  echo "Installed: $BIN/vrrdiag, $APPS/vrrdiag.desktop (logs -> ~/vrrdiag-logs)"
fi

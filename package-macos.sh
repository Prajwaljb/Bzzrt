#!/bin/sh
# Builds dist/macos/Synchora.app (arm64, macOS 12+) and a zip of it. Run on a Mac with the Xcode command line tools.
#   ./package-macos.sh              Apple silicon build
#   ARCHS="arm64;x86_64" ./package-macos.sh   universal build
set -eu
cd "$(dirname "$0")"
ROOT=$(pwd)
ARCHS=${ARCHS:-arm64}
VERSION=$(sed -n 's/.*kVersion = "\(.*\)";/\1/p' src/common/config.h)

cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 "-DCMAKE_OSX_ARCHITECTURES=$ARCHS"
cmake --build build-mac --parallel

DIST="$ROOT/dist/macos"
APP="$DIST/Synchora.app"
rm -rf "$DIST"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp build-mac/vrrdiag "$APP/Contents/MacOS/vrrdiag"
strip -x "$APP/Contents/MacOS/vrrdiag" 2>/dev/null || true
sed "s/@VERSION@/$VERSION/g" packaging/macos/Info.plist > "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"
# Ad-hoc signature: required for arm64 code; the app is not notarized (see QUICKSTART for opening it).
codesign --force --deep --sign - "$APP"
cp README.md LICENSE docs/VRR_LFC_GUIDE.md packaging/QUICKSTART.txt "$DIST/"
echo "Packaged $APP"

#!/bin/bash
# Builds a sideloadable RetroArch for iPhone with the PCSX ReARMed (PS1)
# and PPSSPP (PSP) cores, both pinned to release tags.
# Usage: pkg/apple/iOS/build-ios.sh
#   (override TEAM / BUNDLE_ID / CORES_DIR / CORE_TAG / PPSSPP_TAG / BIOS_DIR via env)
# Then install: xcrun devicectl device install app --device <udid> pkg/apple/build/ipa/RetroArch.ipa
set -euo pipefail

APPLE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CORES_DIR="${CORES_DIR:-$APPLE_DIR/build/cores}"
CORE_SRC="$CORES_DIR/pcsx_rearmed"
JOBS="$(sysctl -n hw.ncpu)"

# Pinned to a release tag of the core, not its development head
CORE_TAG="${CORE_TAG:-r26l}"
if [ ! -d "$CORE_SRC" ] ; then
    git clone --depth=1 --branch "$CORE_TAG" --recurse-submodules --shallow-submodules \
        https://github.com/libretro/pcsx_rearmed.git "$CORE_SRC"
fi
make -C "$CORE_SRC" -f Makefile.libretro platform=ios-arm64 clean
make -C "$CORE_SRC" -f Makefile.libretro platform=ios-arm64 \
    IOSSDK="$(xcrun --sdk iphoneos --show-sdk-path)" \
    MINVERSION=-miphoneos-version-min=16.0 -j"$JOBS"
cp "$CORE_SRC/pcsx_rearmed_libretro_ios.dylib" "$APPLE_DIR/iOS/modules/"

# PSP: same recipe as libretro's own iOS CI for this core. A local
# checkout next to retroarch/ (the user's own fork) is preferred; it is
# built out of tree, so nothing is written into it. Otherwise the
# release tag is cloned.
PPSSPP_TAG="${PPSSPP_TAG:-v1.20.4}"
if [ -z "${PPSSPP_SRC:-}" ] ; then
    if [ -f "$APPLE_DIR/../../../ppsspp/libretro/libretro.cpp" ] ; then
        PPSSPP_SRC="$(cd "$APPLE_DIR/../../../ppsspp" && pwd)"
    else
        PPSSPP_SRC="$CORES_DIR/ppsspp"
        if [ ! -d "$PPSSPP_SRC" ] ; then
            git clone --depth=1 --branch "$PPSSPP_TAG" --recurse-submodules --shallow-submodules \
                https://github.com/hrydgard/ppsspp.git "$PPSSPP_SRC"
        fi
    fi
fi
PPSSPP_BUILD="$CORES_DIR/ppsspp-build/$(echo "$PPSSPP_SRC" | shasum | cut -c1-8)"
echo "PPSSPP source: $PPSSPP_SRC ($(git -C "$PPSSPP_SRC" describe --tags --always 2>/dev/null))"
cmake -S "$PPSSPP_SRC" -B "$PPSSPP_BUILD" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_C_FLAGS=-DIOS -DCMAKE_CXX_FLAGS=-DIOS -DIOS=ON \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_SYSTEM_PROCESSOR=arm64 -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_TOOLCHAIN_FILE="$PPSSPP_SRC/cmake/Toolchains/ios.cmake" -DLIBRETRO=ON
cmake --build "$PPSSPP_BUILD" --target ppsspp_libretro -- -j"$JOBS"
cp "$(find "$PPSSPP_BUILD" -maxdepth 3 -name ppsspp_libretro.dylib | head -1)" \
    "$APPLE_DIR/iOS/modules/ppsspp_libretro_ios.dylib"

cd "$APPLE_DIR"
rm -rf build/RetroArchPS1.xcarchive build/ipa
xcodebuild -project RetroArch_iOS13.xcodeproj -scheme "RetroArch iOS Release" \
    -configuration Release -destination generic/platform=iOS \
    -xcconfig iOS/Personal.xcconfig \
    ${TEAM:+DEVELOPMENT_TEAM=$TEAM} ${BUNDLE_ID:+IOS_BUNDLE_IDENTIFIER=$BUNDLE_ID} \
    CURRENT_PROJECT_VERSION="$(date +%s)" \
    -archivePath build/RetroArchPS1.xcarchive -allowProvisioningUpdates archive

# Add to the app's assets.zip, which is extracted into Documents/RetroArch,
# so system/ lands in the frontend's system directory:
#  - the user's own BIOS dumps (copyrighted: never commit them)
#  - PPSSPP's data files (fonts, flash0, compat.ini), read from system/PPSSPP
# Only the archived app is touched; export re-signs it.
BIOS_DIR="${BIOS_DIR:-$APPLE_DIR/../../../bios}"
STAGE="$(mktemp -d)"
mkdir -p "$STAGE/system"
if [ -d "$BIOS_DIR" ] ; then
    for f in "$BIOS_DIR"/* ; do
        case "$f" in
            *.[bB][iI][nN]) cp "$f" "$STAGE/system/$(basename "$f" | tr '[:upper:]' '[:lower:]')" ;;
        esac
    done
fi
echo "Bundled BIOS: $(ls "$STAGE/system" | tr '\n' ' ')"
cp -R "$PPSSPP_SRC/assets" "$STAGE/system/PPSSPP"
APP_ASSETS="build/RetroArchPS1.xcarchive/Products/Applications/RetroArch.app/assets.zip"
(cd "$STAGE" && zip -qr "$APPLE_DIR/$APP_ASSETS" system)
rm -rf "$STAGE"

TEAM_ID="${TEAM:-$(sed -n 's/^DEVELOPMENT_TEAM = //p' iOS/Personal.xcconfig)}"
cat > build/ExportOptions.plist <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>method</key><string>debugging</string>
<key>teamID</key><string>$TEAM_ID</string>
<key>signingStyle</key><string>automatic</string>
</dict></plist>
PLIST
xcodebuild -exportArchive -archivePath build/RetroArchPS1.xcarchive \
    -exportPath build/ipa -exportOptionsPlist build/ExportOptions.plist \
    -allowProvisioningUpdates
echo "IPA: $APPLE_DIR/build/ipa/RetroArch.ipa"

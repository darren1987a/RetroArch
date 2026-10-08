#!/bin/bash
# Builds a sideloadable RetroArch for iPhone with the PCSX ReARMed (PS1) core.
# Usage: pkg/apple/iOS/build-ps1.sh   (override TEAM / BUNDLE_ID / CORES_DIR / CORE_TAG via env)
# Then install: xcrun devicectl device install app --device <udid> pkg/apple/build/ipa/RetroArch.ipa
set -euo pipefail

APPLE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CORES_DIR="${CORES_DIR:-$APPLE_DIR/build/cores}"
CORE_SRC="$CORES_DIR/pcsx_rearmed"

# Pinned to a release tag of the core, not its development head
CORE_TAG="${CORE_TAG:-r26l}"
if [ ! -d "$CORE_SRC" ] ; then
    git clone --depth=1 --branch "$CORE_TAG" --recurse-submodules --shallow-submodules \
        https://github.com/libretro/pcsx_rearmed.git "$CORE_SRC"
fi
make -C "$CORE_SRC" -f Makefile.libretro platform=ios-arm64 clean
make -C "$CORE_SRC" -f Makefile.libretro platform=ios-arm64 \
    IOSSDK="$(xcrun --sdk iphoneos --show-sdk-path)" \
    MINVERSION=-miphoneos-version-min=16.0 -j"$(sysctl -n hw.ncpu)"
cp "$CORE_SRC/pcsx_rearmed_libretro_ios.dylib" "$APPLE_DIR/iOS/modules/"

cd "$APPLE_DIR"
rm -rf build/RetroArchPS1.xcarchive build/ipa
xcodebuild -project RetroArch_iOS13.xcodeproj -scheme "RetroArch iOS Release" \
    -configuration Release -destination generic/platform=iOS \
    -xcconfig iOS/Personal.xcconfig \
    ${TEAM:+DEVELOPMENT_TEAM=$TEAM} ${BUNDLE_ID:+IOS_BUNDLE_IDENTIFIER=$BUNDLE_ID} \
    -archivePath build/RetroArchPS1.xcarchive -allowProvisioningUpdates archive

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

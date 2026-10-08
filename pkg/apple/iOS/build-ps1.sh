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
    CURRENT_PROJECT_VERSION="$(date +%s)" \
    -archivePath build/RetroArchPS1.xcarchive -allowProvisioningUpdates archive

# Bundle the user's own BIOS dumps into the app's assets.zip, which is
# extracted into Documents/RetroArch, so they land in system/. Only the
# archived app is touched (export re-signs it); BIOS files are
# copyrighted and must never be committed to the repository.
BIOS_DIR="${BIOS_DIR:-$APPLE_DIR/../../../bios}"
if [ -d "$BIOS_DIR" ] && ls "$BIOS_DIR" | grep -qi '\.bin$' ; then
    STAGE="$(mktemp -d)"
    mkdir -p "$STAGE/system"
    for f in "$BIOS_DIR"/* ; do
        case "$f" in
            *.[bB][iI][nN]) cp "$f" "$STAGE/system/$(basename "$f" | tr '[:upper:]' '[:lower:]')" ;;
        esac
    done
    APP_ASSETS="build/RetroArchPS1.xcarchive/Products/Applications/RetroArch.app/assets.zip"
    (cd "$STAGE" && zip -qr "$APPLE_DIR/$APP_ASSETS" system)
    echo "Bundled BIOS: $(ls "$STAGE/system" | tr '\n' ' ')"
    rm -rf "$STAGE"
fi

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

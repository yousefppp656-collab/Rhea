#!/usr/bin/env bash
# Registers the Rhea (.rhea / .rh) file icon (rhea.icns) and UTI metadata in the current user's macOS LaunchServices database.
# Supports --dry-run to validate paths and plist syntax without mutating ~/Applications.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
PLIST_SRC="$SCRIPT_DIR/Info.plist"
ICNS_SRC="$ROOT_DIR/assets/icons/rhea.icns"
APP_BUNDLE="${RHEA_MACOS_APP_BUNDLE:-$HOME/Applications/RheaFileAssociation.app}"

if [ ! -f "$ICNS_SRC" ]; then
    echo "error: missing $ICNS_SRC" >&2
    exit 1
fi
if [ ! -f "$PLIST_SRC" ]; then
    echo "error: missing $PLIST_SRC" >&2
    exit 1
fi

if [ "${1:-}" = "--dry-run" ]; then
    echo "[DRY-RUN] Verified $PLIST_SRC and $ICNS_SRC -> target $APP_BUNDLE"
    exit 0
fi

mkdir -p "$APP_BUNDLE/Contents/Resources"
cp "$PLIST_SRC" "$APP_BUNDLE/Contents/Info.plist"
cp "$ICNS_SRC" "$APP_BUNDLE/Contents/Resources/rhea.icns"

LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
if [ -x "$LSREGISTER" ]; then
    "$LSREGISTER" -f "$APP_BUNDLE" || true
fi
echo "[OK] Installed Rhea macOS UTI & rhea.icns bundle at $APP_BUNDLE"
echo "Note: macOS Finder may require 'killall Finder' or logging out/in to refresh cached desktop icons."

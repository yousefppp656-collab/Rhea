#!/usr/bin/env bash
# Removes the Rhea (.rhea / .rh) user-level macOS UTI helper bundle.
# Supports --dry-run for safe verification.
set -euo pipefail

APP_BUNDLE="${RHEA_MACOS_APP_BUNDLE:-$HOME/Applications/RheaFileAssociation.app}"
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"

if [ "${1:-}" = "--dry-run" ]; then
    echo "[DRY-RUN] Would unregister and remove $APP_BUNDLE"
    exit 0
fi

if [ -d "$APP_BUNDLE" ]; then
    if [ -x "$LSREGISTER" ]; then
        "$LSREGISTER" -u "$APP_BUNDLE" || true
    fi
    rm -rf "$APP_BUNDLE"
    echo "[OK] Removed $APP_BUNDLE"
else
    echo "[OK] $APP_BUNDLE is not installed."
fi

#!/usr/bin/env bash
# Removes the Rhea MIME type (text/x-rhea) and hicolor icons from the user's Linux desktop directories.
# Supports --dry-run for safe verification.
set -euo pipefail

XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
MIME_PKG_FILE="$XDG_DATA_HOME/mime/packages/text-x-rhea.xml"
HICOLOR_DIR="$XDG_DATA_HOME/icons/hicolor"

if [ "${1:-}" = "--dry-run" ]; then
    echo "[DRY-RUN] Would remove $MIME_PKG_FILE and text-x-rhea icons from $HICOLOR_DIR"
    exit 0
fi

rm -f "$MIME_PKG_FILE"
for sz in 16 24 32 48 64 128 256 512; do
    rm -f "$HICOLOR_DIR/${sz}x${sz}/mimetypes/text-x-rhea.png"
done
rm -f "$HICOLOR_DIR/scalable/mimetypes/text-x-rhea.svg"

if command -v update-mime-database >/dev/null 2>&1; then
    update-mime-database "$XDG_DATA_HOME/mime" || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "$HICOLOR_DIR" || true
fi
echo "[OK] Removed Rhea Linux MIME type and hicolor icons from $XDG_DATA_HOME"

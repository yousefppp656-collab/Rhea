#!/usr/bin/env bash
# Installs the Rhea MIME type (text/x-rhea) and hicolor multi-size icons for Linux desktops.
# Supports --dry-run for safe verification.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
MIME_SRC="$SCRIPT_DIR/text-x-rhea.xml"
ICONS_DIR="$ROOT_DIR/assets/icons"

XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
MIME_PKG_DIR="$XDG_DATA_HOME/mime/packages"
HICOLOR_DIR="$XDG_DATA_HOME/icons/hicolor"

if [ ! -f "$MIME_SRC" ]; then
    echo "error: missing $MIME_SRC" >&2
    exit 1
fi
for sz in 16 24 32 48 64 128 256 512; do
    if [ ! -f "$ICONS_DIR/rhea-icon-${sz}.png" ]; then
        echo "error: missing $ICONS_DIR/rhea-icon-${sz}.png" >&2
        exit 1
    fi
done

if [ "${1:-}" = "--dry-run" ]; then
    echo "[DRY-RUN] Verified $MIME_SRC and hicolor PNGs -> target $XDG_DATA_HOME"
    exit 0
fi

mkdir -p "$MIME_PKG_DIR"
cp "$MIME_SRC" "$MIME_PKG_DIR/text-x-rhea.xml"

for sz in 16 24 32 48 64 128 256 512; do
    mkdir -p "$HICOLOR_DIR/${sz}x${sz}/mimetypes"
    cp "$ICONS_DIR/rhea-icon-${sz}.png" "$HICOLOR_DIR/${sz}x${sz}/mimetypes/text-x-rhea.png"
done
mkdir -p "$HICOLOR_DIR/scalable/mimetypes"
cp "$ICONS_DIR/rhea-file-icon-dark.svg" "$HICOLOR_DIR/scalable/mimetypes/text-x-rhea.svg"

if command -v update-mime-database >/dev/null 2>&1; then
    update-mime-database "$XDG_DATA_HOME/mime" || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "$HICOLOR_DIR" || true
fi
echo "[OK] Installed Rhea Linux MIME type and hicolor icons in $XDG_DATA_HOME"

#!/bin/bash
# Installs the UzAlpha keyboards for the current macOS user.
# Double-click this file in Finder, or run:  bash install.command
set -e
cd "$(dirname "$0")"

DEST="$HOME/Library/Keyboard Layouts"
mkdir -p "$DEST"
rm -rf "$DEST/UzAlpha.bundle"
cp -R "UzAlpha.bundle" "$DEST/"

echo
echo "UzAlpha keyboards copied to: $DEST"
echo
echo "Next:"
echo "  1. Log out and log in again (macOS loads new keyboards at login)."
echo "  2. System Settings > Keyboard > Input Sources > Edit > +"
echo "  3. Choose Uzbek (or Others) and add:"
echo "       Özbekça (lotin, avtomatik)"
echo "       Özbekça (lotin, qölda)"
echo

# Open the Keyboard settings pane (ignored on systems that do not know the URL).
open "x-apple.systempreferences:com.apple.Keyboard-Settings.extension" 2>/dev/null || true

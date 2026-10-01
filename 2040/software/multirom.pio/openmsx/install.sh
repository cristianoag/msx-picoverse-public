#!/bin/sh
# Installs the PicoVerse 2040 MultiROM openMSX script and extension for the current user.
# Usage: sh install.sh [openMSX user directory]
set -e

OPENMSX_USER_DIR="${1:-${OPENMSX_HOME:-$HOME/.openMSX}}"
SRC="$(dirname "$0")/share"

for dir in scripts extensions/PicoVerse_2040; do
	mkdir -p "$OPENMSX_USER_DIR/share/$dir"
	cp "$SRC/$dir/"* "$OPENMSX_USER_DIR/share/$dir/"
	echo "Installed $dir to $OPENMSX_USER_DIR/share/$dir"
done
echo "Restart openMSX: 'PicoVerse 2040 MultiROM' is now in the Extensions menu and 'help picoverse2040' works in the console (F10)."

#!/bin/sh
# Builds the macOS HAL plug-in, installs it, and restarts coreaudiod so it
# picks up the new build. Run from anywhere; paths are resolved relative to
# this script's location.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DRIVER_DIR="$(dirname "$SCRIPT_DIR")"   # src/driver

BUNDLE_NAME="WiMic.driver"
BUILT_BUNDLE="$SCRIPT_DIR/build/$BUNDLE_NAME"
INSTALL_DIR="/Library/Audio/Plug-Ins/HAL"

echo "==> Building $BUNDLE_NAME"
make -C "$DRIVER_DIR" mac

echo "==> Installing to $INSTALL_DIR (requires sudo)"
sudo rm -rf "$INSTALL_DIR/$BUNDLE_NAME"
sudo cp -R "$BUILT_BUNDLE" "$INSTALL_DIR/$BUNDLE_NAME"

echo "==> Restarting coreaudiod"
sudo killall coreaudiod

echo "Done."

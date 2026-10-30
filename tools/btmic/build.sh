#!/bin/bash
# Build the Mac helper as out/btmic.app (own bundle id, so the Bluetooth and
# Accessibility permissions belong to it rather than to the terminal).
set -euo pipefail
cd "$(dirname "$0")"
APP=out/btmic.app
mkdir -p "$APP/Contents/MacOS"
swiftc -O -o "$APP/Contents/MacOS/btmic" btmic.swift
cp Info.plist "$APP/Contents/Info.plist"
codesign -s - -f "$APP" 2>/dev/null
echo "built: $(pwd)/$APP"

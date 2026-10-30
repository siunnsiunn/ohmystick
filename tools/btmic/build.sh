#!/bin/bash
# Build the Mac helper as out/btmic.app (own bundle id, so the Bluetooth and
# Accessibility permissions belong to it rather than to the terminal).
set -euo pipefail
cd "$(dirname "$0")"
APP=out/btmic.app
mkdir -p "$APP/Contents/MacOS"
swiftc -O -o "$APP/Contents/MacOS/btmic" btmic.swift
cp Info.plist "$APP/Contents/Info.plist"
# Signing identity: $BTMIC_SIGN_ID, else the first line of ~/.btmic-sign-id (a certificate name or
# hash from `security find-identity -v -p codesigning`), else ad-hoc. A real identity keeps the
# Bluetooth and Accessibility grants across rebuilds; ad-hoc signing loses them every time.
ID="${BTMIC_SIGN_ID:-$(head -n1 "$HOME/.btmic-sign-id" 2>/dev/null || true)}"
if [ -n "$ID" ]; then
  codesign -s "$ID" -f "$APP"
  echo "signed with: ${ID:0:12}..."
else
  codesign -s - -f "$APP" 2>/dev/null
  echo "ad-hoc signed (Accessibility must be re-added after each build)"
fi
echo "built: $(pwd)/$APP"

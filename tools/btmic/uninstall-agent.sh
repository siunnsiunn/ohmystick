#!/bin/bash
# Stop btmic and remove the LaunchAgent.
set -uo pipefail
PLIST="$HOME/Library/LaunchAgents/local.btmic.plist"
launchctl bootout "gui/$(id -u)" "$PLIST" 2>/dev/null
pkill -x btmic 2>/dev/null
rm -f "$PLIST"   # a file we wrote ourselves; no need for the Trash
echo "removed"

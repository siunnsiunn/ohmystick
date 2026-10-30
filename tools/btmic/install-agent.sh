#!/bin/bash
# Run btmic at login and restart it if it crashes (LaunchAgent local.btmic).
# Needs a build first (build.sh). Remove again with uninstall-agent.sh.
set -euo pipefail
cd "$(dirname "$0")"
EXE="$(pwd)/out/btmic.app/Contents/MacOS/btmic"
PLIST="$HOME/Library/LaunchAgents/local.btmic.plist"
xml() { printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'; }
ENVXML=""
for v in BTMIC_LANG BTMIC_MODEL BTMIC_ENTER BTMIC_KEEP BTMIC_TIMEOUT BTMIC_ONLINE BTMIC_LOG_TEXT; do
  [ -n "${!v:-}" ] && ENVXML="$ENVXML<key>$v</key><string>$(xml "${!v}")</string>"
done
[ -x "$EXE" ] || { echo "no build yet: run build.sh first"; exit 1; }
mkdir -p "$HOME/Library/LaunchAgents"
cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>local.btmic</string>
  <key>ProgramArguments</key><array><string>$(xml "$EXE")</string><string>listen</string></array>
  <key>EnvironmentVariables</key><dict>$ENVXML</dict>
  <key>RunAtLoad</key><true/>
  <!-- restart only after a crash: a duplicate that finds the lock held exits 0 and stays down -->
  <key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>
  <key>ThrottleInterval</key><integer>15</integer>
  <key>StandardOutPath</key><string>/dev/null</string>
  <key>StandardErrorPath</key><string>/dev/null</string>
</dict></plist>
PL
chmod 644 "$PLIST"
pkill -x btmic 2>/dev/null || true
launchctl bootout "gui/$(id -u)" "$PLIST" 2>/dev/null || true
for i in 1 2 3; do
  launchctl bootstrap "gui/$(id -u)" "$PLIST" 2>/dev/null && break
  [ "$i" = 3 ] && { echo "bootstrap failed"; exit 1; }
  sleep 1
done
echo "installed: $PLIST (log: ~/.btmic.log)"

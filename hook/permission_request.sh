#!/bin/bash
# Claude Code PermissionRequest hook -> asks the StickS3 and waits for a button.
#
# Fail-safe contract: the ONLY ways this prints a decision are an explicit,
# id-matched answer from the device. Anything else (no device, timeout, garbage,
# dangerous command without a confirmed long-press) prints nothing and exits 0,
# so Claude Code falls back to its normal on-screen prompt. It never auto-allows.
#
# Wiring (device I/O is two paths so tests can use FIFOs):
#   STICK_TX  file the host writes requests to   (default: STICK_DEV)
#   STICK_RX  file the host reads answers from   (default: STICK_DEV)
#   STICK_DEV serial device, e.g. /dev/cu.usbmodemXXXX
#   STICK_TIMEOUT seconds to wait for a button (default 30)
set -u

TIMEOUT="${STICK_TIMEOUT:-30}"
TX="${STICK_TX:-${STICK_DEV:-}}"
RX="${STICK_RX:-${STICK_DEV:-}}"
MAXLEN=110

TMO="$(command -v timeout || command -v gtimeout || true)"
[ -n "$TMO" ] && command -v jq >/dev/null 2>&1 || exit 0
[ -n "$TX" ] && [ -n "$RX" ] || exit 0
[ -e "$TX" ] && [ -e "$RX" ] || exit 0

INPUT="$(cat)"
TOOL="$(jq -r '.tool_name // "?"' <<<"$INPUT")"

# ---- what to show -----------------------------------------------------------
case "$TOOL" in
  Bash)               RAW="$(jq -r '.tool_input.command // ""' <<<"$INPUT")" ;;
  Write|Edit|MultiEdit|NotebookEdit)
                      RAW="$(jq -r '.tool_input.file_path // .tool_input.notebook_path // ""' <<<"$INPUT")" ;;
  *)                  RAW="$(jq -rc '.tool_input // {} | keys | join(",")' <<<"$INPUT")" ;;
esac

# Redact obvious secrets, drop control characters, collapse whitespace.
clean() {
  LC_ALL=C sed -E \
    -e 's/(sk-[A-Za-z0-9_-]{8})[A-Za-z0-9_-]+/\1…[redacted]/g' \
    -e 's/(gh[pousr]_[A-Za-z0-9]{4})[A-Za-z0-9]+/\1…[redacted]/g' \
    -e 's/([Bb]earer )[A-Za-z0-9._~+\/-]+/\1[redacted]/g' \
    -e 's/((token|secret|password|passwd|api[_-]?key)=)[^ &]+/\1[redacted]/gi' \
    | LC_ALL=C tr -d '\000-\010\013-\037\177' | tr '\n\t' '  ' | tr -s ' '
}
SUMMARY_FULL="$(printf '%s' "$RAW" | clean)"
TRUNC=false
if [ "${#SUMMARY_FULL}" -gt "$MAXLEN" ]; then
  SUMMARY="${SUMMARY_FULL:0:$MAXLEN}"; TRUNC=true
else
  SUMMARY="$SUMMARY_FULL"
fi

# ---- danger: needs a long-press confirm on the device ------------------------
DANGER=false
if [ "$TOOL" = "Bash" ]; then
  if grep -qEi '(^|[;&|[:space:](])(rm|rmdir|sudo|dd|mkfs|chmod|chown|kill|pkill|launchctl)([[:space:]]|$)|git[[:space:]]+(push|reset|clean|checkout[[:space:]]+--|branch[[:space:]]+-D)|wrangler[[:space:]]+(deploy|delete|publish)|(curl|wget)[^|]*\|[[:space:]]*(ba|z)?sh|>[[:space:]]*/(etc|usr|System)|--force|-rf?[[:space:]]' <<<"$RAW"; then
    DANGER=true
  fi
fi
# A summary we cannot show in full is treated as dangerous: never approve unseen text.
[ "$TRUNC" = true ] && DANGER=true

ID="$(uuidgen 2>/dev/null || echo "$$-$RANDOM")"
REQ="$(jq -nc --arg id "$ID" --arg tool "$TOOL" --arg s "$SUMMARY" \
  --argjson danger "$DANGER" --argjson trunc "$TRUNC" --argjson ttl "$TIMEOUT" \
  '{v:1,type:"ask",id:$id,tool:$tool,summary:$s,danger:$danger,truncated:$trunc,ttl:$ttl}')"

# ---- talk to the device ----------------------------------------------------
[ -c "$RX" ] && stty -f "$RX" 115200 raw -echo 2>/dev/null

"$TMO" 3 bash -c 'printf "%s\n" "$1" > "$2"' _ "$REQ" "$TX" 2>/dev/null || exit 0

ANS=""
END=$(( $(date +%s) + TIMEOUT ))
while [ "$(date +%s)" -lt "$END" ]; do
  LEFT=$(( END - $(date +%s) )); [ "$LEFT" -lt 1 ] && break
  # Open RX inside the timed process: a redirect here would block in the shell,
  # outside the timeout, whenever the device never opens its end.
  LINE="$("$TMO" "$LEFT" bash -c 'head -n 1 < "$1"' _ "$RX" 2>/dev/null || true)"
  [ -z "$LINE" ] && continue
  # Ignore anything that is not our answer (late replies for older ids, logs).
  if [ "$(jq -r --arg id "$ID" 'select(.id==$id) | .id' <<<"$LINE" 2>/dev/null)" = "$ID" ]; then
    ANS="$LINE"; break
  fi
done
[ -n "$ANS" ] || exit 0

DECISION="$(jq -r '.decision // ""' <<<"$ANS")"
CONFIRMED="$(jq -r '.confirmed // false' <<<"$ANS")"

emit() { # behavior [message]
  jq -nc --arg b "$1" --arg m "${2:-}" \
    'if $b=="deny" then {hookSpecificOutput:{hookEventName:"PermissionRequest",decision:{behavior:"deny",message:$m}}}
     else {hookSpecificOutput:{hookEventName:"PermissionRequest",decision:{behavior:"allow"}}} end'
}

case "$DECISION" in
  deny)  emit deny "Denied from StickS3" ;;
  allow)
    if [ "$DANGER" = true ] && [ "$CONFIRMED" != "true" ]; then
      exit 0   # dangerous but not long-press confirmed -> normal prompt
    fi
    emit allow ;;
  *) exit 0 ;;
esac

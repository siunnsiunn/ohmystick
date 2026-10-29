#!/bin/bash
# Offline tests: hook <-> fake stick over FIFOs. No hardware, no network.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOOK="$ROOT/hook/permission_request.sh"
T="$(mktemp -d)"; trap 'kill $(jobs -p) 2>/dev/null; trash "$T" 2>/dev/null' EXIT
pass=0; fail=0

run_case() { # name mode tool_json expect(allow|deny|none) [timeout]
  local name="$1" mode="$2" input="$3" expect="$4" tmo="${5:-3}"
  mkfifo "$T/h2d" "$T/d2h"
  FAKE_LOG="$T/log" "$ROOT/sim/fake_stick.sh" "$T/h2d" "$T/d2h" "$mode" &
  local sim=$!
  # keep d2h open for reading so the fake's writes do not block forever
  local out
  out="$(STICK_TX="$T/h2d" STICK_RX="$T/d2h" STICK_TIMEOUT="$tmo" "$HOOK" <<<"$input")"
  kill "$sim" 2>/dev/null; wait "$sim" 2>/dev/null
  trash "$T/h2d" "$T/d2h" 2>/dev/null
  local got
  got="$(jq -r '.hookSpecificOutput.decision.behavior // "none"' <<<"${out:-{\}}" 2>/dev/null)"
  [ -z "$out" ] && got=none
  if [ "$got" = "$expect" ]; then pass=$((pass+1)); echo "ok   $name"
  else fail=$((fail+1)); echo "FAIL $name: expected=$expect got=$got out=$out"; fi
}

BASH_SAFE='{"tool_name":"Bash","tool_input":{"command":"ls -la"}}'
BASH_DANGER='{"tool_name":"Bash","tool_input":{"command":"git push origin main"}}'
BASH_LONG="{\"tool_name\":\"Bash\",\"tool_input\":{\"command\":\"echo $(printf 'a%.0s' $(seq 1 200))\"}}"
WRITE='{"tool_name":"Write","tool_input":{"file_path":"/tmp/x.txt","content":"hi"}}'

run_case "safe + allow button"                 allow   "$BASH_SAFE"   allow
run_case "safe + deny button"                  deny    "$BASH_SAFE"   deny
run_case "write file + allow"                  allow   "$WRITE"       allow
run_case "danger + plain allow -> fallback"    allow   "$BASH_DANGER" none
run_case "danger + long-press confirm -> allow" confirm "$BASH_DANGER" allow
run_case "danger + deny -> deny"               deny    "$BASH_DANGER" deny
run_case "truncated command + plain allow -> fallback" allow "$BASH_LONG" none
run_case "silent device -> fallback"           silent  "$BASH_SAFE"   none 2
run_case "wrong id ignored -> fallback"        wrongid "$BASH_SAFE"   none 2
run_case "garbage ignored -> fallback"         garbage "$BASH_SAFE"   none 2

# No device configured must never decide anything.
out="$(env -u STICK_TX -u STICK_RX -u STICK_DEV "$HOOK" <<<"$BASH_SAFE")"
if [ -z "$out" ]; then pass=$((pass+1)); echo "ok   no device -> fallback"; else fail=$((fail+1)); echo "FAIL no device: $out"; fi

# Secrets must not be sent to the device.
mkfifo "$T/h2d" "$T/d2h"
( head -n1 < "$T/h2d" > "$T/seen" ) &
STICK_TX="$T/h2d" STICK_RX="$T/d2h" STICK_TIMEOUT=1 "$HOOK" \
  <<<'{"tool_name":"Bash","tool_input":{"command":"curl -H \"Authorization: Bearer abc123SECRET\" https://x.test?api_key=hunter2 sk-abcdefgh12345678"}}' >/dev/null
wait 2>/dev/null
if grep -q 'hunter2\|abc123SECRET\|12345678' "$T/seen" 2>/dev/null; then fail=$((fail+1)); echo "FAIL secrets leaked: $(cat "$T/seen")"
else pass=$((pass+1)); echo "ok   secrets redacted"; fi

echo "---- pass=$pass fail=$fail"
[ "$fail" -eq 0 ]

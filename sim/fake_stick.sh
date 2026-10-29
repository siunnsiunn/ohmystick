#!/bin/bash
# Fake StickS3 for tests. Reads one request per line from RX_FIFO (host->device),
# answers on TX_FIFO (device->host) according to MODE:
#   allow | deny | confirm (allow + confirmed:true) | silent | wrongid | garbage
# Usage: fake_stick.sh <host_to_dev_fifo> <dev_to_host_fifo> <mode>
set -u
IN="$1"; OUT="$2"; MODE="$3"

while IFS= read -r line < "$IN"; do
  id="$(jq -r '.id' <<<"$line")"
  case "$MODE" in
    allow)   printf '{"v":1,"id":"%s","decision":"allow"}\n' "$id" > "$OUT" ;;
    deny)    printf '{"v":1,"id":"%s","decision":"deny"}\n' "$id" > "$OUT" ;;
    confirm) printf '{"v":1,"id":"%s","decision":"allow","confirmed":true}\n' "$id" > "$OUT" ;;
    wrongid) printf '{"v":1,"id":"not-%s","decision":"allow","confirmed":true}\n' "$id" > "$OUT" ;;
    garbage) printf 'not json at all\n' > "$OUT" ;;
    silent)  : ;;
  esac
  echo "$line" >> "${FAKE_LOG:-/dev/null}"
done

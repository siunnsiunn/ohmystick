#!/bin/bash
# Start the helper as an app (arguments as for the binary). Log: ~/.btmic.log
# BTMIC_* variables set in the calling shell (LANG, MODEL, ENTER, KEEP) are passed on.
cd "$(dirname "$0")"
envargs=()
for v in BTMIC_LANG BTMIC_MODEL BTMIC_ENTER BTMIC_KEEP; do
  [ -n "${!v:-}" ] && envargs+=(--env "$v=${!v}")
done
open -n "${envargs[@]}" out/btmic.app --args "${@:-listen}"
echo "started; follow with: tail -f ~/.btmic.log"

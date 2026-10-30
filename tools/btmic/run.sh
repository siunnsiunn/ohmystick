#!/bin/bash
# Start the helper as an app (arguments as for the binary). Log: ~/.btmic.log
cd "$(dirname "$0")"
: > "$HOME/.btmic.log"
open -n out/btmic.app --args "${@:-listen}"
echo "started; follow with: tail -f ~/.btmic.log"

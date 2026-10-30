#!/bin/bash
# Build the Mac helper into tools/btmic/out/btmic (ignored by git).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out
swiftc -O -o out/btmic btmic.swift
echo "built: $(pwd)/out/btmic"

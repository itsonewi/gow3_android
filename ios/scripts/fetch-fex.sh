#!/usr/bin/env bash
# Fetch the FEX fork the iOS build needs.
#
# The iOS support (16 KB host pages, SMC traps, the allocator hooks the pool
# relies on) lives in Madeira's fork, https://github.com/willfaust/FEX, not in
# upstream FEX-Emu. This replaces the stale submodule checkout (which points at
# upstream) with the fork, shallow, including the bundled dependencies
# (fmt, xxhash, SoftFloat-3e) the CMake build compiles.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FEX_DIR="$ROOT/third_party/FEX"
FEX_URL="${FEX_URL:-https://github.com/willfaust/FEX.git}"
# Madeira builds the iOS branch, not main: ios-port-2607 carries the Apple
# platform support (16 KB host pages, SMC traps, iOS in CMakeLists).
FEX_REF="${FEX_REF:-ios-port-2607}"

if [[ -d "$FEX_DIR/.git" ]] && git -C "$FEX_DIR" remote get-url origin 2>/dev/null | grep -q 'willfaust/FEX'; then
    echo "FEX fork already present at $FEX_DIR; checking out $FEX_REF"
    git -C "$FEX_DIR" fetch --depth 1 origin "$FEX_REF"
    git -C "$FEX_DIR" checkout -q -f FETCH_HEAD
else
    rm -rf "$FEX_DIR"
    mkdir -p "$(dirname "$FEX_DIR")"
    git clone --depth 1 --recurse-submodules --shallow-submodules \
        --branch "$FEX_REF" "$FEX_URL" "$FEX_DIR"
fi

git -C "$FEX_DIR" rev-parse HEAD

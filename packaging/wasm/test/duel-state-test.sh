#!/bin/sh
set -eu
top=$(cd "$(dirname "$0")/../../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -I"$top/src" \
    "$top/packaging/wasm/test/duel-state-test.c" "$top/src/Gameplay/sithDuelState.c" -o "$out/test"
"$out/test"

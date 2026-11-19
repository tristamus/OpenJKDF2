#!/bin/sh
# Builds and runs steptime-test.c on this tree's own code for the milliseconds
# each fixed physics step stands for (sithTime_StepMs in
# src/Gameplay/sithTime.c): what an item's respawn and every other
# whole-millisecond timer counts down by. sithTime.c can't be compiled on its
# own, so the function is cut out of it first. From the top of the source
# tree:
#
#   sh packaging/wasm/test/steptime-test.sh [other source tree]
#
# Another tree runs the same tests on that tree's code.
set -eu

top=$(cd "$(dirname "$0")/../../.." && pwd)
src=${1:-$top}/src
time=Gameplay/sithTime.c
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

# Prints the lines of src/$1 from the first one that starts with $2 and is
# not a declaration (ending in ";") to the next one that starts with "}".
cut_out() {
    if ! awk -v a="$2" '
            !p && index($0, a) == 1 && !/;[ \t]*$/ { p = 1; print; next }
            p { print }
            p && index($0, "}") == 1 { done = 1; exit }
            END { exit !done }' "$src/$1"
    then
        echo "steptime-test.sh: no \"$2\" in src/$1" >&2
        exit 1
    fi
}

{
    if ! grep -E '^flex_d_t sithTime_physicsRolloverMs = 0.0;' "$src/$time"; then
        echo "steptime-test.sh: no sithTime_physicsRolloverMs in src/$time" >&2
        exit 1
    fi
    cut_out "$time" 'uint32_t sithTime_StepMs('
} > "$out/steptime-code.inc"

cc -std=c11 -O2 -Wall -Werror -I"$out" -o "$out/steptime-test" "$top/packaging/wasm/test/steptime-test.c" -lm
"$out/steptime-test"

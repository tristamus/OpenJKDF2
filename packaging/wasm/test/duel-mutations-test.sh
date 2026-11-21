#!/bin/sh
# Round-trip five engine thing-update formats through their production writers
# and receivers. Only transport, world lookup, and downstream callbacks are
# supplied by the fixture.
set -eu
top=$(cd "$(dirname "$0")/../../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
touch "$out/globals.h"
for function in DuelOwner Pos ProcessPos UpdateState ProcessStateUpdate Attachment ProcessAttachment DestroyThing ProcessDestroyThing Death ProcessDeath; do
    awk -v fn="sithDSSThing_${function}(" '
        /^[a-zA-Z]/ && index($0, fn) && !/;[[:space:]]*$/ { copying=1 }
        copying { print }
        copying && /^}/ { done=1; exit }
        END { if (!done) exit 1 }
    ' "$top/src/Dss/sithDSSThing.c" >> "$out/handlers.inc" || {
        echo "Missing engine definition: sithDSSThing_$function" >&2
        exit 1
    }
done
cc -std=gnu11 -O2 -Wall -Werror -Wno-unused-variable -Wno-unused-value \
   -Wno-strict-aliasing -fno-strict-aliasing -DPLATFORM_WSRELAY -DQOL_IMPROVEMENTS \
   -DNO_JK_MMAP -DLINUX -DSTDSOUND_OPENAL -I"$top/src" -I"$out" \
   "$top/packaging/wasm/test/duel-mutations-test.c" -o "$out/test"
"$out/test"

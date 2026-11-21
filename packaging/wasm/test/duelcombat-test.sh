#!/bin/sh
# Build the production combat module against real SithThing/SithPlayer types.
# Only the engine globals and runtime callbacks are supplied by the fixture.
set -eu
top=$(cd "$(dirname "$0")/../../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cat > "$out/globals.h" <<'HEADER'
#pragma once
#include "types.h"
extern int sithNet_isMulti, sithWeapon_8BD024, sithInventory_bUnkPower, sithInventory_g_bSendDeactivateMessage;
extern SithControlBinding sithInventory_powerKeybinds[SITHINVENTORY_NUM_POWERKEYBINDS];
extern SithPlayer jkPlayer_playerInfos[JKPLAYER_NUM_INFOS];
extern SithThing *sithPlayer_g_pLocalPlayerThing;
extern SithInventoryType sithInventory_g_aTypes[SITHBIN_NUMBINS];
HEADER
# Link the original blade-trace loop to an ordered two-contact fixture.
sed -n '/^void  *jkSaber_UpdateCollision2(/,/^}/p' "$top/src/Gameplay/jkSaber.c" > "$out/blade-trace.inc"
cc -std=gnu11 -O2 -Wall -Werror -Wno-parentheses -DLINUX -DNO_JK_MMAP -DPLATFORM_WSRELAY \
   -DQOL_IMPROVEMENTS -DSTDSOUND_OPENAL -I"$out" -I"$top/src" \
   "$top/packaging/wasm/test/duelcombat-test.c" \
   "$top/src/Gameplay/sithDuelCombat.c" -o "$out/test"
"$out/test"

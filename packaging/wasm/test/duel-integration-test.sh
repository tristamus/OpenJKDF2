#!/bin/sh
# Three independent production duel adapters over loopback TCP. No game data,
# SDK, browser, or generated build files are needed; only cc and Python 3.
set -eu
top=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cat > "$out/globals.h" <<'HEADER'
#pragma once
#include "types.h"
extern int32_t sithNet_isMulti, sithNet_isServer, sithNet_MultiModeFlags;
extern int32_t sithNet_bNeedsFullThingSyncForLeaveJoin, sithMulti_newPlayerId;
extern uint32_t sithMulti_curWelcomePlayerNum, jkPlayer_maxPlayers;
extern int stdComm_dplayIdSelf, stdComm_bIsServer, playerThingIdx;
extern int sithWeapon_8BD024, sithInventory_bUnkPower, sithInventory_g_bSendDeactivateMessage;
extern SithControlBinding sithInventory_powerKeybinds[SITHINVENTORY_NUM_POWERKEYBINDS];
extern SithPlayer jkPlayer_playerInfos[JKPLAYER_NUM_INFOS];
extern SithThing *sithPlayer_g_pLocalPlayerThing;
extern SithInventoryType sithInventory_g_aTypes[SITHBIN_NUMBINS];
extern jkEpisodeLoad jkEpisode_mLoad;
HEADER
"${CC:-cc}" -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-variable \
    -DPLATFORM_WSRELAY -DPLATFORM_POSIX -DLINUX -DNO_JK_MMAP \
    -DQOL_IMPROVEMENTS -DSTDSOUND_OPENAL -I"$out" -I"$top/src" \
    "$top/packaging/wasm/test/duel-integration-peer.c" \
    "$top/src/Gameplay/sithDuel.c" "$top/src/Gameplay/sithDuelState.c" \
    "$top/src/Gameplay/sithDuelCombat.c" -lm -o "$out/peer"
"${PYTHON:-python3}" "$top/packaging/wasm/test/duel-integration-test.py" \
    --peer "$out/peer" --repo "$top" --workdir "$out" "$@"

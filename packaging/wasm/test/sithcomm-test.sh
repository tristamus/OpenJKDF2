#!/bin/sh
# Builds and runs sithcomm-test.c on this tree's own reliable-message code, in
# each shape the game builds it: desktop, browser (PLATFORM_WSRELAY), and the
# retro ports' heap buffer (SITHCOMM_HEAP_MSGBUF). That code can't be
# compiled on its own, so its structs and functions are cut out of the
# sources first. From the top of the source tree:
#
#   sh packaging/wasm/test/sithcomm-test.sh [other source tree]
#
# Another tree runs the same tests on that tree's code.
set -eu

top=$(cd "$(dirname "$0")/../../.." && pwd)
src=${1:-$top}/src
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

# Prints the lines of src/$1 from the first one that starts with $2 and is
# not a declaration (ending in ";") to the next one that starts with $3 ("}"
# if not given).
cut_out() {
    end=${3-}
    [ -n "$end" ] || end='}'
    if ! awk -v a="$2" -v b="$end" '
            !p && index($0, a) == 1 && !/;[ \t]*$/ { p = 1; print; next }
            p { print }
            p && index($0, b) == 1 { done = 1; exit }
            END { exit !done }' "$src/$1"
    then
        echo "sithcomm-test.sh: no \"$2\" in src/$1" >&2
        exit 1
    fi
}

{
    cut_out types_enums.h 'enum DSS_ID'
    grep '^#define DPSYS_DELETEPLAYER ' "$src/types_win_enums.h"
    cut_out types.h 'typedef struct net_msg'
    cut_out types.h 'typedef struct sithCogMsg_Pair'
    cut_out types.h 'typedef struct SithMessage'
    grep -E '^#define NETMSG_POP([US]16|S32|STR)\(' "$src/Dss/sithMulti.h"
} > "$out/sithcomm-types.inc"

{
    cut_out Devices/sithComm.c '#ifdef SITHCOMM_HEAP_MSGBUF' '#endif'
    cut_out Devices/sithComm.c 'int sithComm_SendMsgToPlayer('
    cut_out Devices/sithComm.c 'int sithMessage_ProcessMessages('
    cut_out Devices/sithComm.c 'void sithComm_ForgetSender('
    cut_out Devices/sithComm.c 'void sithComm_ForgetRecipient('
    cut_out Devices/sithComm.c 'void sithComm_SyncWithPlayers('
    cut_out Devices/sithComm.c 'void sithComm_ClearMsgTmpBuf('
    cut_out Devices/sithComm.c 'int sithComm_cogMsg_Reset('
    cut_out Win95/stdComm.c 'int stdComm_Recv('
    cut_out Win95/stdComm.c 'int stdComm_SendToPlayer('
    cut_out Gameplay/sithPlayer.c 'uint32_t sithPlayer_GetPlayerNum('
    cut_out Gameplay/sithPlayer.c 'void sithPlayer_Startup('
    cut_out Dss/sithMulti.c 'int sithMulti_ProcessSyncPlayers('
} > "$out/sithcomm-code.inc"

for shape in "" PLATFORM_WSRELAY SITHCOMM_HEAP_MSGBUF; do
    echo "== ${shape:-desktop}"
    cc -std=c11 -O2 -Wall -Werror ${shape:+-D$shape} -I"$out" -o "$out/sithcomm-test" \
       "$top/packaging/wasm/test/sithcomm-test.c"
    "$out/sithcomm-test"
done

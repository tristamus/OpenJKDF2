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

# Prints the lines of src/$1 from the one that starts with $2 and is not a
# declaration (ending in ";", maybe with a comment) to the next one that starts with $3 ("}" if not
# given). There must be one such line, not right after an #if, #ifdef,
# #ifndef, #elif or #else: otherwise a copy the game doesn't build could be
# tested in place of the one it does. Only for a $2 that is itself an #ifdef
# the first is taken, as several SITHCOMM_HEAP_MSGBUF blocks open the same
# way; the buffer's is the first, and any other fails to build in that shape.
cut_out() {
    end=${3-}
    [ -n "$end" ] || end='}'
    case $2 in '#'*) one=0 ;; *) one=1 ;; esac
    st=0
    awk -v a="$2" -v b="$end" -v one=$one '
            index($0, a) == 1 && !/;[ \t]*(\/\/.*)?$/ {
                n++
                if (last ~ /^[ \t]*#[ \t]*(if|el)/)
                    guarded = 1
                if (n == 1) { p = 1; print; last = $0; next }
            }
            p {
                print
                if (index($0, b) == 1) { p = 0; done = 1; if (!one) exit }
            }
            !/^[ \t]*(\/\/.*)?$/ { last = $0 }
            END { exit (!done ? 2 : !one ? 0 : n > 1 ? 3 : guarded ? 4 : 0) }' "$src/$1" || st=$?
    case $st in
    0) ;;
    3) echo "sithcomm-test.sh: more than one \"$2\" in src/$1" >&2; exit 1 ;;
    4) echo "sithcomm-test.sh: \"$2\" in src/$1 comes right after an #if" >&2; exit 1 ;;
    *) echo "sithcomm-test.sh: no \"$2\" in src/$1" >&2; exit 1 ;;
    esac
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

# The test builds the shapes below and no others, so code under an #if on any
# other macro would be tested one way only, maybe not the game's.
other=$(grep -hE '^[ \t]*#[ \t]*(if|elif)' "$out/sithcomm-types.inc" "$out/sithcomm-code.inc" |
        grep -vxE '[ \t]*#[ \t]*ifn?def[ \t]+(PLATFORM_WSRELAY|SITHCOMM_HEAP_MSGBUF)[ \t]*(//.*)?' || true)
if [ -n "$other" ]; then
    printf 'sithcomm-test.sh: an #if in the code it cuts out is not one of its shapes:\n%s\n' "$other" >&2
    exit 1
fi

for shape in "" PLATFORM_WSRELAY SITHCOMM_HEAP_MSGBUF; do
    echo "== ${shape:-desktop}"
    cc -std=c11 -O2 -Wall -Werror ${shape:+-D$shape} -I"$out" -o "$out/sithcomm-test" \
       "$top/packaging/wasm/test/sithcomm-test.c"
    "$out/sithcomm-test"
done

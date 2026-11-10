#!/bin/sh
# Builds and runs passwords-test.c on this tree's own join code
# (src/Platform/Networking/WSRelay/stdComm_WSRelay.c): what the join sends
# for a room's password, what the lobby's answers become, and when the join
# screen (src/Gui/jkGUIMultiplayer.c) puts up the list. That file only
# builds for the browser, so the structs, statics and functions the test
# needs are cut out of the sources first. From the top of the source tree:
#
#   sh packaging/wasm/test/passwords-test.sh [other source tree]
#
# Another tree runs the same tests on that tree's code.
set -eu

top=$(cd "$(dirname "$0")/../../.." && pwd)
src=${1:-$top}/src
relay=Platform/Networking/WSRelay/stdComm_WSRelay.c
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

# Prints the lines of src/$1 from the first one that starts with $2, as a
# whole word, and is not a declaration (ending in ";") to the next one that
# starts with $3 ("}" if not given).
cut_out() {
    end=${3-}
    [ -n "$end" ] || end='}'
    if ! awk -v a="$2" -v b="$end" '
            !p && index($0, a) == 1 && (a !~ /[A-Za-z0-9_]$/ || substr($0, length(a) + 1, 1) !~ /[A-Za-z0-9_]/) &&
                !/;[ \t]*$/ { p = 1; print; next }
            p { print }
            p && index($0, b) == 1 { done = 1; exit }
            END { exit !done }' "$src/$1"
    then
        echo "passwords-test.sh: no \"$2\" in src/$1" >&2
        exit 1
    fi
}

# Prints the lines of src/$1 after the first one that is $2, leading blanks
# aside, up to the next one that starts with $3.
cut_after() {
    if ! awk -v a="$2" -v b="$3" '
            p && index($0, b) == 1 { done = 1; exit }
            p { print }
            !p { t = $0; sub(/^[ \t]+/, "", t); if (t == a) p = 1 }
            END { exit !done }' "$src/$1"
    then
        echo "passwords-test.sh: no \"$2\" in src/$1" >&2
        exit 1
    fi
}

# Prints the declaration of each static variable named, as src/$relay has it.
statics() {
    for name in "$@"; do
        if ! grep -E "^static [^(]*[ *]$name( =|\\[|;)" "$src/$relay"; then
            echo "passwords-test.sh: no static $name in src/$relay" >&2
            exit 1
        fi
    done
}

{
    cut_out types_enums.h 'enum sithMultiSessionFlags' '};'
    cut_out types.h 'typedef struct GUID_idk' '} GUID_idk;'
    echo '#pragma pack(push, 4)'
    cut_out types.h 'typedef struct jkMultiEntry' '} jkMultiEntry;'
    echo '#pragma pack(pop)'
    grep -E '^#define STDCOMM_WSRELAY_[A-Z_]+ \(' "$src/Platform/Networking/WSRelay/stdComm_WSRelay.h"
} > "$out/passwords-types.inc"

{
    grep -E '^#define WSRELAY_[A-Z_]+ \(' "$src/$relay"
    cut_out "$relay" '#define wsrelay_verbosePrintf' '    ;'
    statics wsrelay_recvBuf wsrelay_bHosting wsrelay_bHostPending wsrelay_bJoining \
            wsrelay_bJoined wsrelay_bHostLeft wsrelay_bSeatLost wsrelay_msecRelayLost wsrelay_joinError \
            wsrelay_room wsrelay_id wsrelay_numDisconnected wsrelay_bNetLive wsrelay_msecNamesSent wsrelay_aSwing \
            wsrelay_aLeftPending wsrelay_numLeftPending wsrelay_numWelcomed wsrelay_storedEntryEnum \
            wsrelay_storedTailEnum wsrelay_pendingRoom wsrelay_bListReceived wsrelay_numListed wsrelay_aListRooms \
            wsrelay_aListEntries wsrelay_aListTails wsrelay_numShown wsrelay_aShownRooms wsrelay_aShownTails \
            wsrelay_aListHits wsrelay_aShownHits wsrelay_aListPw wsrelay_aShownPw \
            wsrelay_bListAsked wsrelay_msecListAsked wsrelay_bListDropped wsrelay_bListGaveUp wsrelay_bListMots \
            wsrelay_bListForget wsrelay_bListUnshown
    cut_out "$relay" 'static int wsrelay_EntryHits('
    cut_out "$relay" 'static int wsrelay_HexNibble('
    cut_out "$relay" 'static int wsrelay_DecodeHex('
    cut_out "$relay" 'static int wsrelay_EntryFromHex('
    cut_out "$relay" 'static int wsrelay_SendText('
    cut_out "$relay" 'static int wsrelay_SendJoin('
    cut_out "$relay" 'static void wsrelay_ResetSession('
    cut_out "$relay" 'static void wsrelay_HandleList('
    cut_out "$relay" 'static void wsrelay_HandleText('
    cut_out "$relay" 'static void wsrelay_PumpControl('
    cut_out "$relay" 'static void wsrelay_DropListAsk('
    cut_out "$relay" 'static int wsrelay_EnsureOpen('
    cut_out "$relay" 'void stdComm_WSRelay_Startup('
    cut_out "$relay" 'void stdComm_CloseConnection('
    cut_out "$relay" 'int DirectPlay_EnumSessions2('
    cut_out "$relay" 'void stdComm_WSRelay_ForgetList('
    cut_out "$relay" 'int stdComm_Open('
    cut_out "$relay" 'static void wsrelay_ShowList('
    cut_out "$relay" 'int stdComm_EnumSessions('
    cut_out "$relay" 'int stdComm_WSRelay_ListReady('
    cut_out "$relay" 'int stdComm_WSRelay_LobbyChecksPassword('
} > "$out/passwords-code.inc"

# The join screen opening, after it clears the last visit's selection.
cut_after Gui/jkGUIMultiplayer.c 'memset(&jkGui_guid_556040, 0, sizeof(GUID));' \
    '#if defined(PLATFORM_WSRELAY) && defined(QOL_IMPROVEMENTS)' > "$out/passwords-open.inc"

cc -std=c11 -O2 -Wall -Werror -DPLATFORM_WSRELAY -I"$out" -o "$out/passwords-test" "$top/packaging/wasm/test/passwords-test.c"
"$out/passwords-test"

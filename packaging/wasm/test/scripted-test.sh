#!/bin/sh
# Builds and runs scripted-test.c on this tree's own code for a level whose
# script keeps the score: the four calls a script has for a player's score,
# kills, deaths and suicides (src/Cog/sithCogFunctionPlayer.c), and what the
# match record makes of them for the lobby
# (src/Platform/Networking/WSRelay/stdComm_WSRelay.c), and the count it keeps
# of a player's deaths by one of the level's own characters. Neither file can
# be compiled on its own, so the functions the test needs are cut out of the
# sources first. From the top of the source tree:
#
#   sh packaging/wasm/test/scripted-test.sh [other source tree]
#
# Another tree runs the same tests on that tree's code.
set -eu

top=$(cd "$(dirname "$0")/../../.." && pwd)
src=${1:-$top}/src
relay=Platform/Networking/WSRelay/stdComm_WSRelay.c
cog=Cog/sithCogFunctionPlayer.c
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
        echo "scripted-test.sh: no \"$2\" in src/$1" >&2
        exit 1
    fi
}

# Prints the declaration of each static variable named, as src/$relay has it.
statics() {
    for name in "$@"; do
        if ! grep -E "^static [^(]*[ *]$name( =|\\[|;)" "$src/$relay"; then
            echo "scripted-test.sh: no static $name in src/$relay" >&2
            exit 1
        fi
    done
}

{
    grep -E '^#define STDCOMM_WSRELAY_(SCRIPTED|KIND)_[A-Z]+ +\(' "$src/Platform/Networking/WSRelay/stdComm_WSRelay.h"
    grep -E '^ +SITH_THING_(ACTOR|PLAYER) += ' "$src/types_enums.h" | sed 's/^ *\([A-Z0-9_]*\) *= *\([0-9a-fx]*\),.*/#define \1 (\2)/'
    grep -E '^ +MULTIMODEFLAG_(TEAMS|4|SCORELIMIT) = ' "$src/types_enums.h" | sed 's/^ *\([A-Z0-9_]*\) = \([0-9a-fx]*\),.*/#define \1 (\2)/'
    # The match record's functions, as the header declares them.
    grep -E '^void stdComm_WSRelay_Match(Start|Kill|Scores|Scripted|End)\(' "$src/Platform/Networking/WSRelay/stdComm_WSRelay.h"
} > "$out/scripted-types.inc"

{
    # The script's side, as the browser build has it.
    cut_out "$cog" '#ifdef PLATFORM_WSRELAY' '#endif'
    cut_out "$cog" 'void sithCogFunctionPlayer_SetPlayerScore('
    cut_out "$cog" 'void sithCogFunctionPlayer_SetPlayerKills('
    cut_out "$cog" 'void sithCogFunctionPlayer_SetPlayerKilled('
    cut_out "$cog" 'void sithCogFunctionPlayer_SetPlayerSuicides('
    # The match record's.
    statics wsrelay_matchKind wsrelay_bMatchOn wsrelay_matchScripted wsrelay_aMatchByLevelId wsrelay_aMatchByLevel
    cut_out "$relay" 'void stdComm_WSRelay_MatchScores('
    cut_out "$relay" 'void stdComm_WSRelay_MatchScripted('
    cut_out "$relay" 'void stdComm_WSRelay_MatchStart('
    cut_out "$relay" 'void stdComm_WSRelay_MatchKill('
    cut_out "$relay" 'void stdComm_WSRelay_MatchEnd('
} > "$out/scripted-code.inc"

cc -std=c11 -O2 -Wall -Werror -DPLATFORM_WSRELAY -I"$out" -o "$out/scripted-test" "$top/packaging/wasm/test/scripted-test.c"
"$out/scripted-test"

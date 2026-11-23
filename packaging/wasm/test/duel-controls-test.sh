#!/bin/sh
# The production registration, menu enumeration, binding, profile and input
# functions run with a temporary profile and synthetic hardware input.
set -eu
top=$(cd "$(dirname "$0")/../../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
src="$top/src/Devices/sithControl.c"

# Extract the named definitions complete, preserving their feature guards.
cut_function() {
    awk -v prefix="$1" '
        index($0,prefix) == 1 && $0 !~ /;/ { matches++; active=1 }
        active { print }
        active && /^}/ { active=0 }
        END { if (matches != 1 || active) exit 1 }
    ' "${2:-$src}" || { echo "Missing or ambiguous definition: $1" >&2; exit 1; }
}

{
    cut_function 'int stdConffile_ReadArgsFromStr(' "$top/src/General/stdConffile.c"
    cut_function 'int stdConffile_ReadArgs(' "$top/src/General/stdConffile.c"
    cut_function 'int stdControl_ReadKey(' "$top/src/Platform/Common/stdControl.c"
    cut_function 'static const char *sithControl_aFunctionStrs['
    cut_function 'void sithControl_Reset('
    cut_function 'void sithControl_RegisterControlFunctions('
    cut_function 'stdControlKeyInfoEntry* sithControl_BindControl('
    cut_function 'void sithControl_UnbindControl('
    cut_function 'void sithControl_DefaultHelper('
    cut_function 'int sithControl_IsKeyBound('
    echo '#ifdef PLATFORM_WSRELAY'
    echo 'typedef struct sithCommandBind sithCommandBind;'
    cut_function 'typedef struct sithCommandBind' "$top/src/Main/sithCommand.c"
    echo 'static sithCommandBind *sithCommand_pBinds;'
    cut_function 'int sithCommand_IsKeyBound(' "$top/src/Main/sithCommand.c"
    cut_function 'int sithDuel_Chat(' "$top/src/Gameplay/sithDuel.c"
    cut_function 'void jkQuakeConsole_ExecuteCommand(' "$top/src/Main/jkQuakeConsole.c"
    cut_function 'void sithCommand_HandleBinds(' "$top/src/Main/sithCommand.c"
    cut_function 'static void sithControl_DefaultDuelBinding('
    echo '#endif'
    cut_function 'void sithControl_RegisterKeyboardBindings('
    cut_function 'void sithControl_RegisterMouseBindings('
    cut_function 'void sithControl_FixNumpadTurn('
    cut_function 'int sithControl_ReadConf('
    cut_function 'int sithControl_WriteConf('
    cut_function 'int sithControl_GetKey('
    cut_function 'stdControlKeyInfo* sithControl_EnumBindings('
    echo '#ifdef PLATFORM_WSRELAY'
    cut_function 'static void sithControl_DuelKey('
    echo '#endif'
} > "$out/control-functions.inc"

# Keep the tested helper on the live player path before the preparation lock.
# An acceptance by the host can enter preparation inside the key callback.
cut_function 'int sithControl_HandlePlayer(' > "$out/player.c"
awk '
    /sithControl_DuelKey\(player\)/ { calls++; dispatch = NR }
    /sithDuelCombat_Phase\(player\) == 1/ { lock = NR }
    /sithControl_PlayerMovement\(player\)/ { movement = NR }
    END { exit !(calls == 1 && dispatch < lock && lock < movement) }
' "$out/player.c" || { echo "Duel input is missing or follows player movement." >&2; exit 1; }

for shape in browser browser-legacy native native-legacy; do
    flags=
    case "$shape" in browser*) flags=-DPLATFORM_WSRELAY ;; esac
    case "$shape" in *-legacy) ;; *) flags="$flags -DQOL_IMPROVEMENTS" ;; esac
    "${CC:-cc}" -std=gnu11 -O1 -Wall -Werror -Wno-unused-variable -Wno-unused-value -Wno-unused-result \
        -Wno-parentheses -DLINUX -DNO_JK_MMAP -DSTDSOUND_OPENAL $flags -I"$out" -I"$top/src" \
        "$top/packaging/wasm/test/duel-controls-test.c" -o "$out/test"
    "$out/test" "$top/resource/ui/openjkdf2.uni"
done

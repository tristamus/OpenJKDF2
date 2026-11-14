#!/bin/sh
# Builds and runs playlist-test.c: level playlists (src/Main/jkPlaylist.c)
# and the Playlist screen (src/Gui/jkGUIPlaylist.c) on stand-ins for the
# game's menu system and episodes and for the page (playlist-test-mock.js),
# with Emscripten and Node, and with AddressSanitizer for reads and writes
# out of bounds and leaks. Needs emcc on the PATH and the engine built once
# in the build folder, for its generated headers. From the top of the
# source tree:
#
#   sh packaging/wasm/test/playlist-test.sh [build folder, build_emcc if not given]
set -eu

top=$(cd "$(dirname "$0")/../../.." && pwd)
build=${1:-$top/build_emcc}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

emcc -std=gnu11 -fshort-wchar -O1 -gsource-map -fno-builtin-wcslen -Wall -Wno-unused-variable -Wno-parentheses -Wno-missing-braces \
    -fsanitize=address -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node -sEXIT_RUNTIME=1 \
    -sEXPORTED_RUNTIME_METHODS=stringToUTF8,UTF8ToString \
    -DARCH_WASM -DLINUX -DLINUX_TMP -DNO_JK_MMAP -DPLATFORM_POSIX -DPLATFORM_WSRELAY -DQOL_IMPROVEMENTS \
    -DSDL2_RENDER -DSTDSOUND_OPENAL -DTARGET_CAN_JKGM \
    -I"$top/src" -I"$build/generated" \
    --pre-js "$top/packaging/wasm/test/playlist-test-mock.js" \
    "$top/src/Main/jkPlaylist.c" "$top/src/Gui/jkGUIPlaylist.c" "$top/src/wprintf.c" \
    "$top/packaging/wasm/test/playlist-test.c" \
    -o "$out/playlist-test.js"
node "$out/playlist-test.js"

#!/bin/sh
# Builds and runs library-menu-test.c: the Level Library menu
# (src/Gui/jkGUILibrary.c) on stand-ins for the game's menu system and for
# the page (library-menu-mock.js), with Emscripten and Node, and with
# AddressSanitizer for reads and writes out of bounds and leaks. Needs
# emcc on the PATH and the engine built once in the build folder, for its
# generated headers. -fshort-wchar and -fno-builtin-wcslen as the engine has
# them: Emscripten's wcslen counts 4-byte letters. From the top of the
# source tree:
#
#   sh packaging/wasm/test/library-menu-test.sh [build folder, build_emcc if not given]
set -eu

top=$(cd "$(dirname "$0")/../../.." && pwd)
build=${1:-$top/build_emcc}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

emcc -std=gnu11 -fshort-wchar -O1 -gsource-map -fno-builtin-wcslen -Wall -Wno-unused-variable -Wno-parentheses -Wno-missing-braces \
    -fsanitize=address -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node -sEXIT_RUNTIME=1 \
    -DARCH_WASM -DLINUX -DLINUX_TMP -DNO_JK_MMAP -DPLATFORM_POSIX -DPLATFORM_WSRELAY -DQOL_IMPROVEMENTS \
    -DSDL2_RENDER -DSTDSOUND_OPENAL -DTARGET_CAN_JKGM \
    -I"$top/src" -I"$build/generated" \
    --pre-js "$top/packaging/wasm/test/library-menu-mock.js" \
    "$top/src/Gui/jkGUILibrary.c" "$top/packaging/wasm/test/library-menu-test.c" \
    -o "$out/library-menu-test.js"
node "$out/library-menu-test.js"

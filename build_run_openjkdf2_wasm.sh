#!/bin/sh

# Run in OpenJKDF2 directory:
# python3 -m http.server
# Navigate to http://localhost:8000/wasm_out/index.html

export OPENJKDF2_RELEASE_COMMIT=$(git log -1 --format="%H")
export OPENJKDF2_RELEASE_COMMIT_SHORT=$(git rev-parse --short=8 HEAD)
export EMSCRIPTEN_ROOT=$(em-config EMSCRIPTEN_ROOT)

mkdir -p wasm_out
mkdir -p wasm_out/jk1
mkdir -p wasm_out/mots

rm -f wasm_out/openjkdf2.js
rm -f wasm_out/openjkdf2.wasm
rm -f wasm_out/openjkdf2.data
rm -f wasm_out/openjkdf2_data.data
rm -f wasm_out/openjkdf2_data.js
rm -f wasm_out/index.html
# The JSPI engine (built below): an old one left here would be loaded instead
# of this build in browsers with JSPI.
rm -rf wasm_out/jspi
rm -rf wasm_out/jk1/resource/shaders
#mkdir -p wasm_out/jk1/resource/shaders
rm -rf wasm_out/mots/resource/shaders
#mkdir -p wasm_out/mots/resource/shaders
#cp resource/shaders/* wasm_out/jk1/resource/shaders
#cp resource/shaders/* wasm_out/mots/resource/shaders

#rm -rf build_emcc
mkdir -p build_emcc && pushd build_emcc
cmake .. --toolchain $(pwd)/../cmake_modules/toolchain_wasm.cmake
cmake .. --toolchain $(pwd)/../cmake_modules/toolchain_wasm.cmake
make -j1 VERBOSE=1
popd

# The JSPI engine, which the page loads from jspi/ where the browser has JSPI
# (cmake_modules/toolchain_wasm.cmake).
mkdir -p build_emcc_jspi && pushd build_emcc_jspi
cmake .. --toolchain $(pwd)/../cmake_modules/toolchain_wasm.cmake -DOPENJKDF2_WASM_JSPI=ON
make -j1 VERBOSE=1
popd

cp packaging/wasm/index.html wasm_out/index.html
cp build_emcc/openjkdf2.js wasm_out/openjkdf2.js
cp build_emcc/openjkdf2.wasm wasm_out/openjkdf2.wasm
mkdir -p wasm_out/jspi
cp build_emcc_jspi/openjkdf2.js wasm_out/jspi/openjkdf2.js
cp build_emcc_jspi/openjkdf2.wasm wasm_out/jspi/openjkdf2.wasm
# Only present when configured with -DOPENJKDF2_WASM_PRELOAD_DATA=ON (local testing;
# the package contains game assets, never publish it). Otherwise the page asks the
# player for their own game folder.
if [ -f build_emcc/openjkdf2_data.data ]; then
    cp build_emcc/openjkdf2_data.js wasm_out/openjkdf2_data.js
    cp build_emcc/openjkdf2_data.data wasm_out/openjkdf2_data.data
    # The JS is minified to one line, so short-circuit the test instead of commenting out the rest of the line
    gsed -i -E 's/var hasByteServing ?= ?/var hasByteServing=false\&\&/g' wasm_out/openjkdf2.js
fi

# Update maxthomas.dev/openjkdf2, if you're me
whodis=$(whoami)
if [ "$whodis"  = 'maxamillion' ]; then
    cp wasm_out/openjkdf2.wasm ../shinyquagsire23.github.io/openjkdf2
    cp wasm_out/index.html ../shinyquagsire23.github.io/openjkdf2
    cp wasm_out/openjkdf2.js ../shinyquagsire23.github.io/openjkdf2
    cp wasm_out/openjkdf2_data.data ../shinyquagsire23.github.io/openjkdf2
    cp wasm_out/openjkdf2_data.js ../shinyquagsire23.github.io/openjkdf2
fi
set(CMAKE_SYSTEM_NAME Emscripten)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(CMAKE_C_COMPILER   "emcc" CACHE STRING "C compiler" FORCE)
set(CMAKE_CXX_COMPILER "em++" CACHE STRING "C++ compiler" FORCE)
set(CMAKE_AR           "emar" CACHE STRING "C archiver" FORCE)

# OPENJKDF2_WASM_JSPI=ON builds the engine for browsers with JSPI (WebAssembly
# JavaScript Promise Integration): the browser pauses the game natively where
# it waits (Window.c's EM_ASYNC_JS, emscripten_sleep), so Asyncify's
# instrumentation of most of the engine's code goes (plat_wasm.cmake). Its
# setjmp/longjmp (libpng's error handling) go through Wasm exception handling,
# not Emscripten's JavaScript invoke_* trampolines: under JSPI those are
# WebAssembly.Suspending imports, and the game can't be paused while one (a
# JavaScript frame) is on the stack, as under loadPngImage's
# png_create_read_struct in MotS ("SuspendError: trying to suspend JS frames").
# So the engine (plat_wasm.cmake), libpng and zlib (build_libpng.cmake,
# build_zlib.cmake, which pass it on to those ExternalProjects every
# configure) are all compiled and linked with -sSUPPORT_LONGJMP=wasm, also when
# the option changes in a build folder that's already set up. Declared here so
# that it's known before project(). packaging/wasm/test/jspi-check.mjs checks
# a built JSPI engine for such imports.
set(OPENJKDF2_WASM_JSPI OFF CACHE BOOL "Build the JSPI engine instead of the Asyncify one")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Cache entry, not a normal variable: CMakeLists.txt later declares
# set(PLAT_WASM FALSE CACHE ...), and under cmake_minimum_required(3.20)
# (CMP0126 OLD) creating that cache entry drops a same-named normal variable,
# so a fresh configure used to fail with "Unknown platform!".
set(PLAT_WASM TRUE CACHE BOOL "Emscripten WASM target" FORCE)
set(SDL2_MIXER_INCLUDE_DIR "")

message( STATUS "Emscripten toolchain invoked" )

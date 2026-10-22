# The web build: the engine, the shell's command table, and a small exported
# API, compiled with Emscripten. Run `web/build.sh`. It has no raptor (a
# built-in Turtle and N-Triples parser stands in), no threads, and no
# sanitizers. The native build is the one in the top-level CMakeLists.txt.
cmake_minimum_required(VERSION 3.20)
project(ontodb_web LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
file(GLOB_RECURSE ENGINE ${ROOT}/src/*.cpp)

add_executable(ontodb_wasm
  ${ENGINE}
  ${ROOT}/tools/shell/shell.cpp
  ${ROOT}/tools/shell/session.cpp
  ${ROOT}/tools/shell/commands.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ontodb_wasm.cpp)
target_include_directories(ontodb_wasm PRIVATE ${ROOT}/src/include ${ROOT}/tools)
target_compile_definitions(ontodb_wasm PRIVATE ONTODB_NO_RAPTOR)
target_compile_options(ontodb_wasm PRIVATE -Wall -Wextra -Wno-unused-private-field -O2 -msimd128)
target_link_options(ontodb_wasm PRIVATE
  -O2
  -sMODULARIZE=1 -sEXPORT_NAME=OntoDB
  -sEXPORTED_FUNCTIONS=_ontodb_exec,_ontodb_query,_ontodb_reset,_malloc,_free
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,FS,UTF8ToString,stringToUTF8,lengthBytesUTF8
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB
  -sFILESYSTEM=1 -sEXIT_RUNTIME=0 -sENVIRONMENT=web,worker,node
  -fexceptions)
target_compile_options(ontodb_wasm PRIVATE -fexceptions)

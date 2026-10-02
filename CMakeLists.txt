cmake_minimum_required(VERSION 3.20)
project(ontodb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(RAPTOR2 REQUIRED IMPORTED_TARGET raptor2)

include(FetchContent)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

file(GLOB_RECURSE ONTODB_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)

add_library(ontodb ${ONTODB_SOURCES})
target_include_directories(ontodb PUBLIC ${CMAKE_SOURCE_DIR}/src/include)
target_link_libraries(ontodb PUBLIC PkgConfig::RAPTOR2)
target_compile_options(ontodb PRIVATE -Wall -Wextra -Werror -Wno-unused-private-field)

add_library(ontodb_shell
  tools/shell/shell.cpp
  tools/shell/session.cpp)
target_include_directories(ontodb_shell PUBLIC ${CMAKE_SOURCE_DIR}/tools)
target_link_libraries(ontodb_shell PUBLIC ontodb)
target_compile_options(ontodb_shell PRIVATE -Wall -Wextra -Werror -Wno-unused-private-field)

add_executable(ontodb_cli tools/shell/main.cpp)
target_link_libraries(ontodb_cli PRIVATE ontodb_shell)
target_compile_options(ontodb_cli PRIVATE -Wall -Wextra -Werror -Wno-unused-private-field)
set_target_properties(ontodb_cli PROPERTIES OUTPUT_NAME ontodb)

file(GLOB_RECURSE ONTODB_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/test/*.cpp)
add_executable(ontodb_test ${ONTODB_TEST_SOURCES})
target_include_directories(ontodb_test PRIVATE ${CMAKE_SOURCE_DIR}/tools ${CMAKE_SOURCE_DIR}/test)
target_link_libraries(ontodb_test PRIVATE ontodb_shell GTest::gtest)
target_compile_options(ontodb_test PRIVATE -Wall -Wextra -Werror -Wno-unused-private-field)
target_compile_definitions(ontodb_test PRIVATE ONTODB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

function(ontodb_sanitize target)
  if(ONTODB_SANITIZER)
    target_compile_options(${target} PRIVATE -fsanitize=${ONTODB_SANITIZER} -fno-omit-frame-pointer -g)
    target_link_options(${target} PRIVATE -fsanitize=${ONTODB_SANITIZER})
  endif()
endfunction()

ontodb_sanitize(ontodb)
ontodb_sanitize(ontodb_shell)
ontodb_sanitize(ontodb_cli)
ontodb_sanitize(ontodb_test)

enable_testing()
add_test(NAME ontodb_test COMMAND ontodb_test)

cmake_minimum_required(VERSION 3.20)
project(Slipstream CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform backends compile to nothing on the wrong OS, so glob everything.
file(GLOB_RECURSE SLIPSTREAM_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(slipstream ${SLIPSTREAM_SOURCES})
target_include_directories(slipstream PRIVATE src)

if(MSVC)
    target_compile_options(slipstream PRIVATE /W4 /WX /permissive- /EHs-c- /GR- /utf-8
                           $<$<CONFIG:Release>:/fp:fast>)
    target_compile_definitions(slipstream PRIVATE _HAS_EXCEPTIONS=0 _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(slipstream PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
endif()

enable_testing()
add_test(NAME selftest COMMAND slipstream --selftest)
add_test(NAME nettest_clean COMMAND slipstream --nettest 3600)
add_test(NAME nettest_lossy COMMAND slipstream --nettest 3600 --lag 80 --jitter 30 --loss 10)
add_test(NAME nettest_desync_detected COMMAND slipstream --nettest 1800 --lag 40 --loss 3 --desync-at 1000)
add_test(NAME botrace_finishes COMMAND slipstream --botrace 42)

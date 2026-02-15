cmake_minimum_required(VERSION 3.20)
project(JxrAutoCleaner VERSION 1.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match libultrahdr's static CRT setting (/MT instead of /MD)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# libultrahdr - build as static library
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
option(UHDR_BUILD_TESTS "Build libultrahdr tests" OFF)
option(UHDR_BUILD_BENCHMARK "Build libultrahdr benchmarks" OFF)
option(UHDR_BUILD_EXAMPLES "Build libultrahdr examples" OFF)
option(UHDR_BUILD_DEPS "Build libultrahdr deps" ON)
add_subdirectory(third_party/libultrahdr)

# Main executable (WIN32 = subsystem:windows, no console)
add_executable(JxrAutoCleaner WIN32
    src/main.cpp
    src/Converter.cpp
    src/SystemCheck.cpp
    src/FileWatcher.cpp
    src/resources.rc
)

target_include_directories(JxrAutoCleaner PRIVATE
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libultrahdr
)

target_link_libraries(JxrAutoCleaner PRIVATE
    uhdr-static
    windowscodecs
    ole32
    shlwapi
    shell32
)

# For ComPtr
target_compile_definitions(JxrAutoCleaner PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

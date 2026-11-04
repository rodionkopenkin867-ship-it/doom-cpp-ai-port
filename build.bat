@echo off
rem Cross-platform build (Windows): configure and build with CMake.
rem Requires CMake plus a compiler toolchain (e.g. Zig cc / mingw target) on PATH.
cmake -B build
cmake --build build --config Release

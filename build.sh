#!/bin/sh
# Cross-platform build (Linux): configure and build with CMake + SDL2.
# Requires: sudo apt install build-essential cmake libsdl2-dev
set -e
cd "$(dirname "$0")"
cmake -B build
cmake --build build -j

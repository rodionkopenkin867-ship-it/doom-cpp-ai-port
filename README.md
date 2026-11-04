<div align="center">

# 🚀 DOOM C++ Engine Port (Cross-Platform)

[![Status](https://img.shields.io/badge/Status-v1.1--beta-brightgreen?style=for-the-badge)](#)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-blue?style=for-the-badge)](#)
[![Language](https://img.shields.io/badge/Language-C%2B%2B-00599C?style=for-the-badge&logo=c%2B%2B)](#)
[![License](https://img.shields.io/badge/License-GPLv2-red?style=for-the-badge)](#)

**An experimental, fully playable port of the original 1993 DOOM engine to C++.**  
Written end-to-end by an autonomous AI agent (OpenCode + GLM 5.3 Flash).

</div>

---

## 🌟 Features

*   **100% C++ Codebase:** Translated from the original 1993/1994 `DOOM.C` source drop, with all x64 architecture pointer issues fixed.
*   **Cross-Platform:** Native support for both Windows and Linux.
*   **Full Music & SFX:** Custom audio backends. MUS lumps are converted to MIDI on the fly and played natively.
*   **Modern Controls:** WASD movement, mouse look, and left-click to shoot.
*   **CMake Build System:** Unified and easy to compile on any system.

| Platform | Video / Input | Audio | Status |
| :--- | :--- | :--- | :--- |
| **Windows** | Win32 GDI | `winmm` (waveOut) | ✅ Fully Working |
| **Linux** | SDL2 | SDL2_mixer | ✅ Fully Working |

## ⚠️ Important: You Need a WAD File

This repository **DOES NOT** include any copyrighted game assets. The engine requires an IWAD file to run. 

1. Download the free **Shareware** version (`doom1.wad`) from [DoomWiki](https://doomwiki.org/wiki/Shareware).
2. Place `doom1.wad` in the **same folder** as the executable.
3. Launch the game.

*(Alternatively, use `doom.wad` from Steam/GOG for the full registered version).*

## 🛠️ How to Build

**Windows Requirements:** [Zig](https://ziglang.org) and [CMake](https://cmake.org/).
```bat
git clone https://github.com/rodionkopenkin867-ship-it/doom-cpp-ai-port.git
cd doom-cpp-ai-port
build.bat

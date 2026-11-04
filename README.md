# DOOM C++ Port (AI-Generated) 🤖👹

An experimental port of the original DOOM engine to C++, written **end-to-end by an autonomous AI agent** (OpenCode + GLM 5.3 Flash). 

> **Status: v1.0-beta (Fully Playable on Windows)**. The game runs, renders, plays sound, and can load levels without crashing. 

## ✨ Features
- **100% C++ Codebase**: Translated from the original 1993/1994 `DOOM.C` source drop.
- **Native Win32 GDI Renderer** (Windows): Authentic 320x200 software rendering.
- **Working Sound Engine** (Windows): Custom `winmm waveOut` backend for SFX and music.
- **Modern Controls**: WASD movement, mouse look, and left-click to shoot.

## ⚠️ IMPORTANT: You need a WAD file
This repository **DOES NOT** include any game assets. You need an IWAD file.
1. Download the free **Shareware** version (`doom1.wad`): [Download here](https://doomwiki.org/wiki/Shareware)
2. Place `doom1.wad` in the same folder as the executable.
3. Run the game.

## 🛠️ How to Build

### Windows
Run `build.bat` (uses Zig and CMake).

### Linux (SDL2)
1. Install SDL2: `sudo apt install libsdl2-dev`
2. Run `chmod +x build.sh && ./build.sh` (uses CMake).

*Linux (SDL2): Video, input, and SFX are fully implemented. Music (MUS->MIDI) is currently WIP. Build via ./build.sh.*

## 📂 Project Structure
- `src/` — Portable C++ sources
- `src/i_video_w32.cpp` / `i_sound_w32.cpp` — Windows native backends
- `src/i_video_sdl.cpp` / `i_sound_sdl.cpp` — Linux SDL2 backends
- `CMakeLists.txt` — Cross-platform build configuration

## 📝 Known Quirks
- **Retro Pixelation**: Original 320x200 software renderer.
- **Map Load Times**: Takes a few seconds due to unoptimized WAD parsing.

## 📜 Disclaimer & License
> This is an experimental AI port for educational purposes. It is not intended to replace GZDoom.

The WAD game data, DOOM logo, and original assets remain the property of **id Software / ZeniMax Media**. 
The source code is licensed under the **GNU General Public License v2 (GPLv2)**.

---

## 🇷🇺 Русская версия

# Порт DOOM на C++ (Сгенерировано ИИ) 🤖👹

Экспериментальный порт оригинального движка DOOM на язык C++, написанный **полностью автономным ИИ-агентом** (OpenCode + GLM 5.3 Flash). 

> **Статус: v1.0-beta (Полностью играбельно на Windows)**. 

### ⚠️ ВАЖНО: Нужен WAD-файл
Скачайте бесплатный `doom1.wad` по [этой ссылке](https://doomwiki.org/wiki/Shareware) и положите рядом с игрой.

### 🛠️ Как собрать
**Windows:** Запустите `build.bat` (требуется Zig и CMake).
**Linux:** Установите SDL2, затем запустите `chmod +x build.sh && ./build.sh`.

*Linux (SDL2): Видео, ввод и звуковые эффекты (SFX) полностью работают. Музыка (MUS->MIDI) пока в разработке. Сборка через ./build.sh.*

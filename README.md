# DOOM C++ Port (AI-Generated)

An experimental C++ port of DOOM, generated end-to-end by an autonomous AI agent
(**OpenCode + GLM 5.3 Flash**), running on a modest PC (*Intel i3, 8 GB RAM*).

This is not a rewrite of an existing port — the C++ translation, build system and
Win32 backends were produced by the agent working from the original 1993/1994
`DOOM.C`/`.H` source drop.

---

## Status

| Subsystem | State |
| --- | --- |
| Game logic (C++ port of `d_main`, `p_*`, `g_game`, ...) | ✅ Working |
| Rendering | ✅ Software renderer (`r_*`), output via **Win32 GDI** |
| Sound | ✅ **winmm `waveOut`** backend |
| Known bug | ⚠️ Crash during `R_Init` (texture lump initialization) |

The `R_Init` crash is the main open issue — it happens while texture/patch lumps
are being cached right after WAD loading, before the title screen is shown.
**Pull requests are very welcome!** A minimal repro is: build, place any valid
`DOOM.WAD` next to `doom.exe`, run.

## Building

The build uses [`zig c++`](https://ziglang.org) as a portable clang + mingw-w64
toolchain — **no MSVC installation required**.

```bat
git clone <this-repo>
cd doom-cpp
build.bat
```

Or directly with PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Both scripts work from any working directory and produce `doom.exe` in the
repo root. The compiler is resolved in this order:

1. `zig` already on your `PATH` (from [ziglang.org](https://ziglang.org/download/)),
2. the uv-managed toolchain: `uv tool install ziglang`.

Options used: `-std=gnu++98 -fno-exceptions -fno-rtti` (the original codebase's
style does not rely on C++ exceptions or RTTI), linked against
`user32 / gdi32 / ws2_32 / winmm`.

## Running / Getting a WAD

The engine requires an **IWAD** file, which is *not* distributed with this repo
(it has its own proprietary license). Place one of the following next to
`doom.exe` (or set the `DOOMWADDIR` environment variable):

- `doom.wad` — registered / retail DOOM
- `doom1.wad` — shareware DOOM (free to download)
- `doom2.wad`, `plutonia.wad`, `tnt.wad`, `doomu.wad`, `doom2f.wad`

You can download the free **shareware** episode here:
<https://doomwiki.org/wiki/Shareware>

If no WAD is found, the program prints
`WAD not found. Please place doom.wad in this folder.` and exits cleanly —
no crash, no half-initialized state.

Extras:

- PWAD mods placed in a `mods\` folder next to the exe are loaded automatically,
- a `.wad` dragged & dropped onto `doom.exe` is loaded as a PWAD,
- config file (`default.cfg`) and savegames are stored in the current directory.

## Project layout

```
src/           portable C++ sources (game logic, renderer, menus, ...)
src/i_video_w32.cpp   Win32 GDI video backend
src/i_sound_w32.cpp   waveOut sound backend
build.ps1      build script (PowerShell)
build.bat      one-click wrapper for build.ps1
```

## Disclaimer

> This is an experimental AI port for educational purposes. It is not a
> replacement for GZDoom.

The WAD game data, DOOM and the DOOM logo remain the property of id Software /
ZeniMax Media. Source code in this repository is licensed under the
**GNU General Public License v2** (see [LICENSE](LICENSE)), as was the original
DOOM source release.

---

## Русская версия

# DOOM C++ Port (сгенерирован ИИ)

Экспериментальный порт DOOM на C++, полностью сгенерированный автономным
ИИ-агентом (**OpenCode + GLM 5.3 Flash**) на не самом мощном ПК
(*Intel i3, 8 ГБ ОЗУ*).

## Статус

| Подсистема | Состояние |
| --- | --- |
| Игровая логика (порт на C++) | ✅ Работает |
| Рендер | ✅ Софтверный рендерер (`r_*`), вывод через **Win32 GDI** |
| Звук | ✅ Бэкенд на **winmm `waveOut`** |
| Известный баг | ⚠️ Вылет на `R_Init` (инициализация текстур) |

Вылет на `R_Init` — главная открытая проблема: происходит при кэшировании
текстурных люмов сразу после загрузки WAD, до экрана заголовка.
**Pull request'ы очень приветствуются!**

## Сборка

Используется [`zig c++`](https://ziglang.org) как переносимый clang + mingw-w64 —
**MSVC не требуется**.

```bat
git clone <этот-репозиторий>
cd doom-cpp
build.bat
```

или напрямую через PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Оба скрипта работают из любой директории и создают `doom.exe` в корне репозитория.
Компилятор ищется сначала в `PATH`, затем в тулчейне из `uv tool install ziglang`.

## Запуск / Где взять WAD

Движку нужен файл **IWAD**, который не распространяется с репозиторием
(собственная лицензия id Software). Положите рядом с `doom.exe` один из файлов:
`doom.wad`, `doom1.wad` (shareware, скачивается бесплатно), `doom2.wad` и т.д.
Бесплатную shareware-версию можно скачать здесь:
<https://doomwiki.org/wiki/Shareware>

Если WAD не найден, программа выводит сообщение
`WAD not found. Please place doom.wad in this folder.` и завершается корректно,
без вылета.

## Дисклеймер

> Это экспериментальный ИИ-порт в образовательных целях. Он не является
> заменой GZDoom.

Права на game data, DOOM и логотип DOOM принадлежат id Software / ZeniMax Media.
Исходный код лицензирован под **GNU GPL v2** (см. [LICENSE](LICENSE)).

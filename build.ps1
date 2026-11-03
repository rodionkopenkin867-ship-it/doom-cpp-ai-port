# Build script for the C++ DOOM port. Uses zig c++ (bundled clang) with the
# mingw-w64 CRT target, so no MSVC installation is required.
$ErrorActionPreference = "Stop"

# Run from the script's own directory, so the build works no matter
# where the user invokes the script from.
Set-Location -LiteralPath $PSScriptRoot

# Locate zig: prefer a zig already on PATH, fall back to the
# uv-managed toolchain (uv tool install ziglang).
$zig = (Get-Command zig -ErrorAction SilentlyContinue).Source
if (-not $zig) {
    $zig = "${env:APPDATA}\uv\tools\ziglang\Lib\site-packages\ziglang\zig.exe"
}
if (-not (Test-Path -LiteralPath $zig)) {
    Write-Error "zig.exe not found. Install it with 'uv tool install ziglang' (or put zig on PATH)."
    exit 1
}

# All translation units, minus the two original Linux backends we replaced
# with i_video_w32.cpp / i_sound_w32.cpp.
$all = Get-ChildItem src\*.cpp | Select-Object -ExpandProperty FullName
$sources = $all | Where-Object {
    (Split-Path $_ -Leaf) -notin @("i_video.cpp", "i_sound.cpp")
}

& $zig c++ `
    -target x86_64-windows-gnu `
    -std=gnu++98 -fno-exceptions -fno-rtti -fpermissive -ferror-limit=0 `
    -fno-sanitize=undefined -fno-sanitize=alignment `
    -DNORMALUNIX -nostdlib++ `
    @($sources) `
    -luser32 -lgdi32 -lws2_32 -lwinmm `
    -o doom.exe 2> build.log
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Output "BUILD OK: doom.exe"

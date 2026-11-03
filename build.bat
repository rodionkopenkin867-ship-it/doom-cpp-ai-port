@echo off
rem Convenience wrapper: run the PowerShell build script from any console,
rem with script execution policy left untouched.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*

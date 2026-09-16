@rem SPDX-License-Identifier: MPL-2.0
@echo off
rem Windows entry point, the counterpart of build.sh. Thin shim: all logic and
rem the option list live in scripts\windows\build.ps1 (build.bat --help).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\windows\build.ps1" %*
exit /b %ERRORLEVEL%

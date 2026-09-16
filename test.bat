@rem SPDX-License-Identifier: MPL-2.0
@echo off
rem Windows entry point, the counterpart of test.sh. Thin shim: the headless
rem environment and ctest defaults live in scripts\windows\test.ps1; extra
rem arguments go to ctest (e.g. test.bat -R Toast).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\windows\test.ps1" %*
exit /b %ERRORLEVEL%

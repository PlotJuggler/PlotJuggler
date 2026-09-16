@rem SPDX-License-Identifier: MPL-2.0
@echo off
rem Windows entry point, the counterpart of run.sh. Thin shim: DLL search path
rem and Qt plugin setup live in scripts\windows\run.ps1; all arguments go to the app.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\windows\run.ps1" %*
exit /b %ERRORLEVEL%

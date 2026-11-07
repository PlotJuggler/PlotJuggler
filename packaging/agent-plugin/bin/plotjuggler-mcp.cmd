@echo off
rem SPDX-License-Identifier: MPL-2.0
rem Windows twin of bin/plotjuggler-mcp: starts `PlotJuggler4 --mcp-bridge` (MCP over stdio).
rem Order: %PLOTJUGGLER4%, PATH, the path the app records at every start, the installer's default folder.
setlocal
set "EXE=%PLOTJUGGLER4%"
if defined EXE if exist "%EXE%" goto run
for %%E in (PlotJuggler4.exe) do set "EXE=%%~$PATH:E"
if defined EXE goto run
set "REC=%LOCALAPPDATA%\PlotJuggler\PlotJuggler4\mcp-instances\executable"
rem The app writes the path as UTF-8: read it in that code page.
chcp 65001 >nul
if exist "%REC%" set /p EXE=<"%REC%"
if defined EXE if exist "%EXE%" goto run
set "EXE=%ProgramFiles%\PlotJuggler4\bin\PlotJuggler4.exe"
if exist "%EXE%" goto run
echo PlotJuggler 4 not found. Install it from https://plotjuggler.io and start it once, or set PLOTJUGGLER4. 1>&2
exit /b 1
:run
"%EXE%" --mcp-bridge
exit /b %errorlevel%

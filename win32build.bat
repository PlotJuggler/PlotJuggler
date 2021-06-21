@echo off
cd %~dp0
SET MINGW_PATH=C:/Qt/Tools/mingw730_64/bin
SET CMAKE_PATH=C:/Qt/Tools/CMake_64/bin
SET CMAKE_PREFIX_PATH=C:/Qt/5.14.2/mingw73_64
SET PATH=%MINGW_PATH%;%CMAKE_PATH%;%PATH%
SET CMAKE_C_COMPILER=%MINGW_PATH%/gcc.exe
SET CMAKE_CXX_COMPILER=%MINGW_PATH%/g++.exe
CLS
:MENU
CLS

ECHO == PLOTJUGGLER BUILDER FOR WINDOWS ==
ECHO -------------------------------------
ECHO 1.  COMPILE
ECHO 2.  CREATE INSTALLER
ECHO 3.  CLEAR BULD FOLDER
ECHO -------------------------------------
ECHO ==========PRESS 'Q' TO QUIT==========
ECHO.

SET INPUT=
SET /P INPUT=Please select a number:

IF /I '%INPUT%'=='1' GOTO Selection1
IF /I '%INPUT%'=='2' GOTO Selection2
IF /I '%INPUT%'=='3' GOTO Selection3
IF /I '%INPUT%'=='Q' GOTO Quit

CLS
GOTO MENU

:Selection1
mkdir build
cd build
cmake -S ".." -DCMAKE_BUILD_TYPE=Release -G "MinGW Makefiles"
cmake --build . --parallel 8
PAUSE
GOTO MENU

:Selection2
RMDIR /S /Q %~dp0\installer\io.plotjuggler.application\data
%CMAKE_PREFIX_PATH%\bin\windeployqt.exe --release --dir %~dp0\installer\io.plotjuggler.application\data %~dp0\build\bin\PlotJuggler.exe
xcopy %~dp0\build\bin\*.* %~dp0\installer\io.plotjuggler.application\data /Y /S /f /z
C:\Qt\Tools\QtInstallerFramework\4.0\bin\binarycreator.exe --offline-only -c %~dp0\installer\config.xml -p %~dp0\installer %~dp0\PlotJugglerInstaller.exe
PAUSE
GOTO MENU

:Selection3
RMDIR /S /Q build
PAUSE
GOTO MENU

:Quit
CLS

ECHO =============THANK YOU===============
ECHO -------------------------------------
ECHO ======PRESS ANY KEY TO CONTINUE======

PAUSE>NUL
EXIT

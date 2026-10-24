@echo off
setlocal
cd /d "%~dp0"

if not defined QT_DIR (
    echo QT_DIR is not set. Point it at the Qt bin directory, for example:
    echo   D:\soft\qt5152\5.15.2\msvc2019_64\bin
    exit /b 1
)

if not exist "%QT_DIR%\qmake.exe" (
    echo qmake.exe not found: "%QT_DIR%\qmake.exe"
    exit /b 1
)

rem windeployqt copies the VC runtime only when VCINSTALLDIR is set.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VCINSTALLDIR if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCINSTALLDIR=%%I\VC\"
)

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b 1

cmake --build build --config Release
if errorlevel 1 exit /b 1

echo.
echo Built: "%~dp0build\Release\ProResPlayer.exe"
exit /b 0

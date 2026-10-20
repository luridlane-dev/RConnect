@echo off
rem Builds dist\RConnect and dist\RConnect-<version>-win64.zip.
rem Needs Visual Studio 2022+ with C++ and Qt 6 (msvc2022_64) with Active Qt.
rem QT_DIR overrides the Qt path.
setlocal EnableExtensions
cd /d "%~dp0"

if not defined QT_DIR set "QT_DIR=C:\Qt\6.11.1\msvc2022_64"
if not exist "%QT_DIR%\bin\Qt6Core.dll" (
    echo [error] Qt not found in "%QT_DIR%". Set QT_DIR to your Qt msvc2022_64 folder.
    exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [error] Visual Studio was not found.
    exit /b 1
)
set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo [error] Visual Studio C++ build tools were not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not defined VCToolsRedistDir (
    echo [error] Could not set up the Visual Studio build environment.
    exit /b 1
)

set "BUILD=%~dp0build-release"
set "DIST=%~dp0dist"
set "OUT=%DIST%\RConnect"
if exist "%BUILD%" rmdir /s /q "%BUILD%"
if exist "%DIST%" rmdir /s /q "%DIST%"
if exist "%DIST%" (
    echo [error] Could not clear "%DIST%". Close RConnect if it is running from there and try again.
    exit /b 1
)

echo == Configuring
cmake -S . -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_DIR%" -DRCONNECT_DEPLOY_QT=OFF >nul || goto :fail
echo == Building
cmake --build "%BUILD%" || goto :fail
set /p VERSION=<"%BUILD%\version.txt"

echo == Collecting files
mkdir "%OUT%\platforms" || goto :fail
copy /y "%BUILD%\RConnect.exe" "%OUT%\" >nul || goto :fail
for %%d in (Qt6Core Qt6Gui Qt6Widgets) do (
    copy /y "%QT_DIR%\bin\%%d.dll" "%OUT%\" >nul || goto :fail
)
copy /y "%QT_DIR%\plugins\platforms\qwindows.dll" "%OUT%\platforms\" >nul || goto :fail

rem App-local VC++ runtime
set "CRT="
for /d %%d in ("%VCToolsRedistDir%x64\Microsoft.VC*.CRT") do set "CRT=%%~d"
if not defined CRT (
    echo [error] VC++ runtime files were not found in "%VCToolsRedistDir%".
    goto :fail
)
for %%f in (msvcp140.dll msvcp140_1.dll msvcp140_2.dll vcruntime140.dll vcruntime140_1.dll) do (
    copy /y "%CRT%\%%f" "%OUT%\" >nul || goto :fail
)
copy /y "%~dp0LICENSE" "%OUT%\LICENSE.txt" >nul || goto :fail
copy /y "%~dp0THIRD-PARTY-NOTICES.txt" "%OUT%\" >nul || goto :fail

echo == Packing
powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%OUT%' -DestinationPath '%DIST%\RConnect-%VERSION%-win64.zip' -Force" || goto :fail

rmdir /s /q "%BUILD%"
echo.
echo Done: %DIST%\RConnect-%VERSION%-win64.zip
exit /b 0

:fail
echo.
echo [error] Release build failed.
exit /b 1

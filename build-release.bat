@echo off
rem Builds dist\RConnect, dist\RConnect-<version>-win64.zip and dist\RConnect-<version>-setup.exe.
rem RConnect.exe and the installer are signed with the code-signing certificate named "Lurid Lane"
rem (the publisher) from the current user's certificate store.
rem Needs Visual Studio 2022+ with C++, Qt 6 (msvc2022_64) with Active Qt, Inno Setup 6 and the Windows SDK.
rem QT_DIR overrides the Qt path, SIGN_CN the certificate name, SKIP_SIGN=1 builds unsigned.
setlocal EnableExtensions
cd /d "%~dp0"

if not defined QT_DIR set "QT_DIR=C:\Qt\6.11.1\msvc2022_64"
if not defined SIGN_CN set "SIGN_CN=Lurid Lane"
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

rem Inno Setup compiler
set "ISCC="
for %%p in ("%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" "%ProgramFiles%\Inno Setup 6\ISCC.exe" "%LocalAppData%\Programs\Inno Setup 6\ISCC.exe") do (
    if not defined ISCC if exist %%p set "ISCC=%%~p"
)
if not defined ISCC (
    echo [error] Inno Setup 6 was not found.
    exit /b 1
)

rem signtool of the newest Windows SDK, and the certificate
set "SIGNTOOL="
if not defined SKIP_SIGN (
    for /d %%v in ("%ProgramFiles(x86)%\Windows Kits\10\bin\10.*") do (
        if exist "%%v\x64\signtool.exe" set "SIGNTOOL=%%v\x64\signtool.exe"
    )
    if not defined SIGNTOOL (
        echo [error] signtool.exe was not found. Install the Windows SDK, or set SKIP_SIGN=1.
        exit /b 1
    )
    powershell -NoProfile -Command "if (-not (Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object { $_.Subject -eq ('CN=' + $env:SIGN_CN) -and $_.HasPrivateKey })) { exit 1 }"
    if errorlevel 1 (
        echo [error] No code-signing certificate "%SIGN_CN%" with a private key in the current user's store.
        exit /b 1
    )
)

rem Every release raises the patch number in VERSION (KEEP_VERSION=1 rebuilds the current one;
rem edit VERSION by hand for a new major or minor). The file is written once the build succeeds.
set "CURRENT="
set /p CURRENT=<"%~dp0VERSION"
for /f "tokens=1-3 delims=. " %%a in ("%CURRENT%") do (
    set "V_MAJOR=%%a"
    set "V_MINOR=%%b"
    set "V_PATCH=%%c"
)
if not defined V_PATCH (
    echo [error] VERSION must hold major.minor.patch, for example 1.2.0.
    exit /b 1
)
set "NEXT=%CURRENT%"
if not defined KEEP_VERSION (
    set /a V_PATCH+=1
    call set "NEXT=%%V_MAJOR%%.%%V_MINOR%%.%%V_PATCH%%"
)
echo Version: %CURRENT% -^> %NEXT%

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
cmake -S . -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_DIR%" -DRCONNECT_DEPLOY_QT=OFF -DRCONNECT_VERSION=%NEXT% >nul || goto :fail
echo == Building
cmake --build "%BUILD%" || goto :fail
set /p VERSION=<"%BUILD%\version.txt"

echo == Collecting files
mkdir "%OUT%\platforms" || goto :fail
copy /y "%BUILD%\RConnect.exe" "%OUT%\" >nul || goto :fail
for %%d in (Qt6Core Qt6Gui Qt6Widgets Qt6Sql) do (
    copy /y "%QT_DIR%\bin\%%d.dll" "%OUT%\" >nul || goto :fail
)
copy /y "%QT_DIR%\plugins\platforms\qwindows.dll" "%OUT%\platforms\" >nul || goto :fail
rem Server import reads the old program's SQLite database
mkdir "%OUT%\sqldrivers" || goto :fail
copy /y "%QT_DIR%\plugins\sqldrivers\qsqlite.dll" "%OUT%\sqldrivers\" >nul || goto :fail

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

call :sign "%OUT%\RConnect.exe" || goto :fail

echo == Packing
powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%OUT%' -DestinationPath '%DIST%\RConnect-%VERSION%-win64.zip' -Force" || goto :fail

echo == Building installer
"%ISCC%" /Qp "/DAppVersion=%VERSION%" "/DSourceDir=%OUT%" "/DOutputDir=%DIST%" "/DIconFile=%~dp0src\assets\icon.ico" "%~dp0installer\RConnect.iss" || goto :fail
call :sign "%DIST%\RConnect-%VERSION%-setup.exe" || goto :fail

rmdir /s /q "%BUILD%"
<nul set /p "=%NEXT%" >"%~dp0VERSION"
echo.
echo Done: RConnect %NEXT%
echo   %DIST%\RConnect-%VERSION%-win64.zip
echo   %DIST%\RConnect-%VERSION%-setup.exe
exit /b 0

:fail
echo.
echo [error] Release build failed.
exit /b 1

rem Signs %1 with SHA-256 and a timestamp (without one when no timestamp server answers).
:sign
if defined SKIP_SIGN (
    echo [warning] SKIP_SIGN is set: %~nx1 stays unsigned.
    exit /b 0
)
echo == Signing %~nx1
for %%t in (http://timestamp.digicert.com http://timestamp.sectigo.com) do (
    "%SIGNTOOL%" sign /s My /n "%SIGN_CN%" /fd sha256 /tr %%t /td sha256 /d "RConnect" %1 >nul 2>&1 && goto :signed
)
echo [warning] No timestamp server answered; signing without a timestamp.
"%SIGNTOOL%" sign /s My /n "%SIGN_CN%" /fd sha256 /d "RConnect" %1 || exit /b 1
:signed
powershell -NoProfile -Command "$s = Get-AuthenticodeSignature -LiteralPath '%~1'; Write-Host ('   ' + $s.SignerCertificate.Subject + ' [' + $s.Status + ']')"
exit /b 0

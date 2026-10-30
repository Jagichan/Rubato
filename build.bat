@echo off
setlocal
rem ---------------------------------------------------------------
rem  Command-line build of Rubato.sln (alternative to the IDE).
rem  Usage:  build.bat [Release|Debug] [x64|x86]
rem  Output: bin\<platform>\<configuration>\Rubato.exe
rem ---------------------------------------------------------------
set "CFG=%~1"
if "%CFG%"=="" set "CFG=Release"
set "PLAT=%~2"
if "%PLAT%"=="" set "PLAT=x64"

where msbuild >nul 2>nul && goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto novs
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto novs

:build
msbuild "%~dp0Rubato.sln" /m /nologo /v:m /p:Configuration=%CFG% /p:Platform=%PLAT%
if errorlevel 1 goto failed
echo.
echo Build OK: %~dp0bin\%PLAT%\%CFG%\Rubato.exe
exit /b 0

:failed
echo.
echo BUILD FAILED - see the messages above.
exit /b 1

:novs
echo.
echo Visual C++ was not found. In the Visual Studio Installer choose
echo Modify and tick "Desktop development with C++", then retry.
exit /b 1

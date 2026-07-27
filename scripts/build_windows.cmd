@echo off
setlocal EnableExtensions

for %%I in ("%~dp0..") do set "VCR_ROOT=%%~fI"
set "VCR_CONFIG=%~1"
if not defined VCR_CONFIG set "VCR_CONFIG=Release"

if /I "%VCR_CONFIG%"=="Debug" (
    set "VCR_BUILD_DIR=%VCR_ROOT%\build\windows-debug"
) else if /I "%VCR_CONFIG%"=="Release" (
    set "VCR_BUILD_DIR=%VCR_ROOT%\build\windows-release"
) else if /I "%VCR_CONFIG%"=="ASan" (
    set "VCR_BUILD_DIR=%VCR_ROOT%\build\windows-asan"
) else (
    echo Usage: %~nx0 [Debug^|Release^|ASan]
    exit /b 2
)

if not exist "%VCR_BUILD_DIR%\build.ninja" (
    call "%VCR_ROOT%\scripts\configure_windows.cmd" "%VCR_CONFIG%"
    if errorlevel 1 exit /b 1
)

set "VCR_CMAKE=C:\Program Files\CMake\bin\cmake.exe"
if not exist "%VCR_CMAKE%" (
    for %%I in (cmake.exe) do set "VCR_CMAKE=%%~$PATH:I"
)

set "VCR_VCVARS="
for %%E in (Community Professional Enterprise BuildTools) do (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCR_VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VCR_VCVARS (
    echo Visual Studio 2022 C++ tools were not found.
    exit /b 1
)
call "%VCR_VCVARS%" amd64
if errorlevel 1 exit /b 1

"%VCR_CMAKE%" --build "%VCR_BUILD_DIR%" --parallel
if errorlevel 1 exit /b 1

"%VCR_CMAKE%" --build "%VCR_BUILD_DIR%" --target test
exit /b %errorlevel%

@echo off
setlocal EnableExtensions

for %%I in ("%~dp0..") do set "VCR_ROOT=%%~fI"
set "VCR_CONFIG=%~1"
if not defined VCR_CONFIG set "VCR_CONFIG=Release"
set "VCR_ASAN_FLAGS="

if /I "%VCR_CONFIG%"=="Debug" (
    set "VCR_BUILD_DIR=%VCR_ROOT%\build\windows-debug"
    set "VCR_CMAKE_BUILD_TYPE=Debug"
    set "VCR_VALIDATION=ON"
) else if /I "%VCR_CONFIG%"=="Release" (
    set "VCR_BUILD_DIR=%VCR_ROOT%\build\windows-release"
    set "VCR_CMAKE_BUILD_TYPE=Release"
    set "VCR_VALIDATION=OFF"
) else if /I "%VCR_CONFIG%"=="ASan" (
    set "VCR_BUILD_DIR=%VCR_ROOT%\build\windows-asan"
    set "VCR_CMAKE_BUILD_TYPE=RelWithDebInfo"
    set "VCR_VALIDATION=ON"
    set "VCR_ASAN_FLAGS=-DCMAKE_CXX_FLAGS=/fsanitize=address"
) else (
    echo Usage: %~nx0 [Debug^|Release^|ASan]
    exit /b 2
)

set "VCR_CMAKE=C:\Program Files\CMake\bin\cmake.exe"
if not exist "%VCR_CMAKE%" (
    for %%I in (cmake.exe) do set "VCR_CMAKE=%%~$PATH:I"
)
if not exist "%VCR_CMAKE%" (
    echo CMake was not found.
    exit /b 1
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

set "VCR_NINJA=%VCR_ROOT%\.tools\ninja.exe"
if not exist "%VCR_NINJA%" (
    echo Downloading Ninja 1.12.1...
    if not exist "%VCR_ROOT%\.tools" mkdir "%VCR_ROOT%\.tools"
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
        "$ErrorActionPreference='Stop';" ^
        "$zip='%VCR_ROOT%\.tools\ninja-win.zip';" ^
        "Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip' -OutFile $zip;" ^
        "Expand-Archive -Path $zip -DestinationPath '%VCR_ROOT%\.tools' -Force;" ^
        "Remove-Item $zip"
    if errorlevel 1 exit /b 1
)

call "%VCR_VCVARS%" amd64
if errorlevel 1 exit /b 1

"%VCR_CMAKE%" ^
    -S "%VCR_ROOT%" ^
    -B "%VCR_BUILD_DIR%" ^
    -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%VCR_NINJA%" ^
    -DCMAKE_BUILD_TYPE="%VCR_CMAKE_BUILD_TYPE%" ^
    -DVCR_ENABLE_VALIDATION="%VCR_VALIDATION%" ^
    -DBUILD_TESTING=ON ^
    %VCR_ASAN_FLAGS%

exit /b %errorlevel%

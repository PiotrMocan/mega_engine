@echo off
setlocal EnableExtensions

for %%I in ("%~dp0..") do set "VCR_ROOT=%%~fI"
set "VCR_EXE=%VCR_ROOT%\build\windows-release\bin\vulkan_editor.exe"

if not exist "%VCR_EXE%" (
    call "%VCR_ROOT%\scripts\build_windows.cmd" Release
    if errorlevel 1 exit /b 1
)

pushd "%VCR_ROOT%"
"%VCR_EXE%" %*
set "VCR_EXIT=%errorlevel%"
popd
exit /b %VCR_EXIT%

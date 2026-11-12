@echo off
call "%~dp0run_windows.cmd" --project "%~dp0..\projects\neon_night_city" --aa smaa --depth-prepass full --front-to-back coarse --width 1920 --height 1080 %*
exit /b %errorlevel%

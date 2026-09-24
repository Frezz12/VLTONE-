@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cmake --build D:\Code\DAW\build-windows --target daw controller_test warp_test
exit /b %ERRORLEVEL%

@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "FLUX_BUILD_DIR=%~dp0..\build-windows"
if exist "%~dp0..\build-flux-verify\CMakeCache.txt" set "FLUX_BUILD_DIR=%~dp0..\build-flux-verify"
cmake --build "%FLUX_BUILD_DIR%" --target %*
exit /b %ERRORLEVEL%

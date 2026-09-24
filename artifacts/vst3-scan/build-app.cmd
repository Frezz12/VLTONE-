@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
"C:\Program Files\CMake\bin\cmake.exe" --build D:\Code\DAW\build-windows --target daw plugin_contract_test plugin_render_state_test
exit /b %ERRORLEVEL%

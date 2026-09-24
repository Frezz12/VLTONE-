@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /std:c++latest /EHsc /MD /DNOMINMAX /I D:\Code\DAW\engine /I D:\Code\DAW\core /I D:\Code\DAW\plugins D:\Code\DAW\artifacts\arturia-preset\host-preset-probe.cpp /FoD:\Code\DAW\artifacts\arturia-preset\host-preset-probe.obj /FeD:\Code\DAW\artifacts\arturia-preset\host-preset-probe.exe /link /STACK:8388608 /LTCG D:\Code\DAW\build-windows\plugins\daw_pluginhost.lib D:\Code\DAW\build-windows\engine\daw_engine.lib D:\Code\DAW\build-windows\core\daw_platform_paths.lib D:\Code\DAW\build-windows\third_party\daw_vst3_pluginterfaces.lib avrt.lib shell32.lib ole32.lib user32.lib gdi32.lib oleaut32.lib uuid.lib advapi32.lib
exit /b %ERRORLEVEL%

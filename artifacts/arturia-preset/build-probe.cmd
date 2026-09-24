@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /std:c++latest /EHsc /MD /DNOMINMAX /I D:\Code\DAW\engine /I D:\Code\DAW\core /I D:\Code\DAW\plugins /I D:\Code\DAW\third_party\vst3 D:\Code\DAW\artifacts\arturia-preset\replay-probe.cpp /FoD:\Code\DAW\artifacts\arturia-preset\replay-probe.obj /FeD:\Code\DAW\artifacts\arturia-preset\replay-probe.exe /link ole32.lib user32.lib /LTCG D:\Code\DAW\build-windows\third_party\daw_vst3_pluginterfaces.lib
exit /b %ERRORLEVEL%






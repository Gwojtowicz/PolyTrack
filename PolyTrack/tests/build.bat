@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d %~dp0
set SDK=%~dp0..\..\iPlug2\Dependencies\IPlug\VST3_SDK
cl /nologo /EHsc /std:c++17 /O2 /I"%SDK%" pthost.cpp "%SDK%\pluginterfaces\base\funknown.cpp" "%SDK%\public.sdk\source\vst\hosting\parameterchanges.cpp" "%SDK%\public.sdk\source\vst\hosting\eventlist.cpp" /Fe:pthost.exe ole32.lib > build.log 2>&1
if errorlevel 1 (type build.log & exit /b 1)
echo built

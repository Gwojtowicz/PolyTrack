@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %~dp0
cl /nologo /O2 /EHsc /std:c++17 /fp:fast /I ..\dsp %1.cpp /Fe%1.exe >build_%1.log 2>&1
if errorlevel 1 (type build_%1.log & exit /b 1)
echo built %1

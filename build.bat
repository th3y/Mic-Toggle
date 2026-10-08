@echo off
setlocal
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -products * -property installationPath`) do set "VSPATH=%%i"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /std:c++17 /O1 /MT /EHsc /utf-8 /DUNICODE /D_UNICODE /GS- main.cpp /Fe:MicToggle.exe /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF user32.lib gdi32.lib comctl32.lib shell32.lib ole32.lib advapi32.lib winmm.lib
del /q main.obj 2>nul

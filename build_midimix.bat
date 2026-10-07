@echo off
setlocal
where cl >nul 2>nul && goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
:build
cl /nologo /std:c11 /W4 /WX /O2 /Fe:midimix.exe midimix.c
exit /b %errorlevel%
:novs
echo No Visual Studio C++ tools found. Run from a Developer Command Prompt, or build with: cc -std=c11 -O2 -o midimix midimix.c
exit /b 1

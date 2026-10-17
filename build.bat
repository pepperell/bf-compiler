@echo off
rem build.bat [debug]  - builds out\bf.exe (console) and out\bfui.exe (Windows UI)
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VSCMD_VER call %VCVARS% >nul
if errorlevel 1 ( echo could not set up the MSVC environment & exit /b 1 )

cd /d "%~dp0"
if not exist out mkdir out

set CXX=/nologo /std:c++20 /utf-8 /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /MT /Fo:out\ /Fd:out\
if "%1"=="debug" ( set CXX=%CXX% /Od /Zi /DDEBUG ) else ( set CXX=%CXX% /O2 /Oi /GL /DNDEBUG )
set LNK=/link /OPT:REF /OPT:ICF
if not "%1"=="debug" set LNK=%LNK% /LTCG

set CORE=src\ir.cpp src\jit.cpp src\interp.cpp src\machine.cpp src\debugger.cpp

cl %CXX% %CORE% src\cli.cpp /Fe:out\bf.exe %LNK% /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1

if exist src\ui.cpp (
  cl %CXX% %CORE% src\terminal.cpp src\help.cpp src\ui.cpp /Fe:out\bfui.exe user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib winmm.lib %LNK% /SUBSYSTEM:WINDOWS
  if errorlevel 1 exit /b 1
)
echo build ok

@echo off
rem Builds ZStudioFFBHost.exe (x64 Release) into ZStudioFFBHost\bin with SDL3.dll and the sample INI beside it.
rem Needs Visual Studio 2022 or later with the C++ workload. No CMake, no NuGet.
setlocal
set HERE=%~dp0
set SDL=%HERE%..\ThirdParty\SDL3
set OUT=%HERE%bin
set VS=
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /O2 /EHsc /W4 /MT /DUNICODE /D_UNICODE /I"%SDL%\include" "%HERE%main.cpp" ^
   /Fo"%OUT%\\" /Fe"%OUT%\ZStudioFFBHost.exe" /link /LIBPATH:"%SDL%\lib\x64" SDL3.lib || exit /b 1
copy /y "%SDL%\lib\x64\SDL3.dll" "%OUT%\" >nul
if not exist "%OUT%\ZStudioFFB.ini" copy /y "%HERE%ZStudioFFB.ini" "%OUT%\" >nul
echo Built %OUT%\ZStudioFFBHost.exe

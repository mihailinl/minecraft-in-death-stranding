@echo off
rem Build ds\build\dsmc.addon64 with MSVC on Windows.
rem Run it from "x64 Native Tools Command Prompt for VS 2022" (or after vcvars64.bat), after tools\fetch_deps.ps1.
setlocal
set "HERE=%~dp0"
set "TP=%HERE%..\third_party"
if not exist "%TP%\reshade-include\reshade.hpp" (
	echo third_party\reshade-include missing: run powershell -ExecutionPolicy Bypass -File tools\fetch_deps.ps1
	exit /b 1
)
if not exist "%HERE%build" mkdir "%HERE%build"
cl /nologo /std:c++20 /O2 /EHsc /MT /LD /W3 /DNOMINMAX /I "%TP%\reshade-include" /I "%TP%\imgui" ^
	"%HERE%src\*.cpp" /Fo"%HERE%build\\" /Fe"%HERE%build\dsmc.addon64" /link ws2_32.lib || exit /b 1
del /q "%HERE%build\*.obj" "%HERE%build\*.lib" "%HERE%build\*.exp" 2>nul
echo built %HERE%build\dsmc.addon64

@echo off
rem Builds LiveDesktop (Release, x64) with the newest Visual Studio / Build Tools that has the C++ workload.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere.exe not found - install Visual Studio 2022+ or Build Tools with the C++ workload.
  exit /b 1
)
rem -find is more reliable than -requires across VS editions/Insiders builds; -all also accepts
rem instances that the installer marks incomplete (common with Insiders/Preview channels).
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -prerelease -products * -find VC\Auxiliary\Build\vcvars64.bat`) do set "VCVARS=%%i"
if not defined VCVARS for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -all -prerelease -products * -find VC\Auxiliary\Build\vcvars64.bat`) do set "VCVARS=%%i"
if not defined VCVARS (
  echo No Visual Studio installation with the x64 C++ tools was found.
  exit /b 1
)
call "%VCVARS%" >nul || exit /b 1

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"

cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build build || exit /b 1
echo.
echo Built: %~dp0build\LiveDesktop.exe

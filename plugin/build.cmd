@echo off
rem Usage: build.cmd [Release|RelWithDebInfo|Debug]   (output: build\<config>\SkinFlow.dll)
rem Needs Visual Studio 2022 (C++ workload, CMake, vcpkg) and the submodules: git submodule update --init --recursive
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio with the C++ workload not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not defined VCPKG_ROOT set "VCPKG_ROOT=%VSDIR%\VC\vcpkg"
if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
set "PATH=%PATH%;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
cd /d "%~dp0"
cmake -S . -B build\%CONFIG% -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ^
	"-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
	-DVCPKG_TARGET_TRIPLET=x64-windows-static-md "-DVCPKG_INSTALLED_DIR=%~dp0build\vcpkg_installed" || exit /b 1
cmake --build build\%CONFIG% || exit /b 1
echo BUILD_OK

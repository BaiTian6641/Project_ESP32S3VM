@echo off
setlocal EnableDelayedExpansion

set "ROOT_DIR=%~dp0"
set "VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
set "QT_PREFIX=C:\Qt\6.6.3\msvc2019_64"
set "SRC_DIR=%ROOT_DIR%gui-esp32s3-simulator"
set "BUILD_DIR=%SRC_DIR%\build-win-ninja"

if not exist "!VSDEVCMD!" (
  echo [build-win] ERROR: VsDevCmd not found at:
  echo   !VSDEVCMD!
  echo [build-win] Install Visual Studio 2022 Build Tools with C++ workload.
  exit /b 1
)

if not exist "!QT_PREFIX!\lib\cmake\Qt6\Qt6Config.cmake" (
  echo [build-win] ERROR: Qt6Config.cmake not found under:
  echo   !QT_PREFIX!
  echo [build-win] Install Qt 6.6.3 msvc2019_64 kit, or update QT_PREFIX in this script.
  exit /b 1
)

call "!VSDEVCMD!" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 (
  echo [build-win] ERROR: Failed to initialize Visual Studio build environment.
  exit /b 1
)

echo [build-win] Configuring...
cmake -S "!SRC_DIR!" -B "!BUILD_DIR!" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="!QT_PREFIX!"
if errorlevel 1 (
  echo [build-win] ERROR: CMake configure failed.
  exit /b 1
)

echo [build-win] Building...
cmake --build "!BUILD_DIR!" -j 6
if errorlevel 1 (
  echo [build-win] ERROR: Build failed.
  exit /b 1
)

echo [build-win] DONE
echo [build-win] Output: !BUILD_DIR!\gui_esp32s3_simulator.exe
exit /b 0

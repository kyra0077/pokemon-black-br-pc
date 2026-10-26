@echo off
rem Builds dist\Pokemon - Black (BR).exe with the MSYS2 MinGW-w64 toolchain.
rem Requirements (MSYS2 MINGW64 shell):
rem   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2 mingw-w64-x86_64-python
setlocal
if "%MSYS2_ROOT%"=="" set "MSYS2_ROOT=C:\msys64"
set "PATH=%MSYS2_ROOT%\mingw64\bin;%PATH%"
cd /d "%~dp0"

if not exist "third_party\melonDS\CMakeLists.txt" (
    echo Baixando o nucleo do melonDS (submodulo)...
    git submodule update --init --depth 1 || goto :error
)
if not exist "Pokemon - Black (BR).nds" (
    echo Coloque "Pokemon - Black (BR).nds" nesta pasta antes de compilar.
    goto :error
)

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || goto :error
cmake --build build --target game || goto :error

echo.
echo Pronto: "%~dp0dist\Pokemon - Black (BR).exe"
exit /b 0

:error
echo Falha na compilacao.
exit /b 1

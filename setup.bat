@echo off
echo === JxrAutoCleaner Build Script ===
echo.

echo [1/4] Initializing submodules...
git submodule update --init --recursive
if errorlevel 1 (echo FAILED & exit /b 1)

echo [2/4] Configuring CMake...
cmake -B build -G "Visual Studio 18 2026" -A x64
if errorlevel 1 (echo FAILED & exit /b 1)

echo [3/4] Building Release...
cmake --build build --config Release
if errorlevel 1 (echo FAILED & exit /b 1)

echo [4/4] Building MSI installer...
wix build installer\Package.wxs -d BuildOutput=build\Release -o build\JxrAutoCleaner-v1.1.msi -ext WixToolset.UI.wixext -ext WixToolset.Util.wixext
if errorlevel 1 (echo FAILED & exit /b 1)

echo.
echo === Build complete! ===
echo   Executable: build\Release\JxrAutoCleaner.exe
echo   Installer:  build\JxrAutoCleaner-v1.1.msi
echo.

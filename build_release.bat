@echo off
setlocal EnableDelayedExpansion

:: Build script for restic-wfx plugin
:: Builds both 32-bit and 64-bit versions and creates release package

:: Get project directory (remove trailing backslash)
set "PROJECT_DIR=%~dp0"
if "!PROJECT_DIR:~-1!"=="\" set "PROJECT_DIR=!PROJECT_DIR:~0,-1!"

set "BUILD64_DIR=!PROJECT_DIR!\build64"
set "BUILD32_DIR=!PROJECT_DIR!\build32"
set "RELEASE_DIR=!PROJECT_DIR!\release"

:: delete BUILD32_DIR:
if exist "!BUILD32_DIR!" rmdir /s /q "!BUILD32_DIR!"
:: delete BUILD64_DIR:
if exist "!BUILD64_DIR!" rmdir /s /q "!BUILD64_DIR!"

:: Extract version from CMakeLists.txt (single source of truth)
set "VERSION="
for /f "usebackq tokens=1,2 delims=()" %%a in ("!PROJECT_DIR!\CMakeLists.txt") do (
    if "%%a"=="set" (
        for /f "tokens=1,2 delims= " %%x in ("%%b") do (
            if "%%x"=="RESTIC_WFX_VERSION" (
                set "VERSION=%%~y"
            )
        )
    )
)
if not defined VERSION set "VERSION=1.0"

:: Toolchain paths - try MSYS2 first, then CLion's bundled MinGW
set "MINGW64_PATH="
set "MINGW32_PATH="

:: Check for complete MSYS2 64-bit toolchain (gcc + make)
if exist "C:\msys64\mingw64\bin\gcc.exe" (
    if exist "C:\msys64\mingw64\bin\mingw32-make.exe" (
        set "MINGW64_PATH=C:\msys64\mingw64\bin"
    )
)

:: Fallback to CLion's bundled 64-bit MinGW
if not defined MINGW64_PATH (
    if exist "C:\Program Files\JetBrains\CLion 2025.3.2\bin\mingw\bin\gcc.exe" (
        set "MINGW64_PATH=C:\Program Files\JetBrains\CLion 2025.3.2\bin\mingw\bin"
    )
)

:: Check for complete MSYS2 32-bit toolchain (gcc + make)
if exist "C:\msys64\mingw32\bin\gcc.exe" (
    if exist "C:\msys64\mingw32\bin\mingw32-make.exe" (
        set "MINGW32_PATH=C:\msys64\mingw32\bin"
    )
)

echo Building restic-wfx version !VERSION!
echo.

:: Check toolchains exist
if not defined MINGW64_PATH (
    echo ERROR: 64-bit MinGW toolchain not found or incomplete
    echo.
    echo Install via MSYS2:
    echo   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make
    echo.
    echo Or ensure CLion 2025.3.2 is installed at default location
    exit /b 1
)
echo Using 64-bit toolchain: !MINGW64_PATH!

if not defined MINGW32_PATH (
    echo ERROR: 32-bit MinGW toolchain not found or incomplete
    echo.
    echo Install via MSYS2:
    echo   pacman -S mingw-w64-i686-gcc mingw-w64-i686-make
    exit /b 1
)
echo Using 32-bit toolchain: !MINGW32_PATH!
echo.

:: Build 64-bit
echo ========================================
echo Building 64-bit plugin...
echo ========================================
set "PATH=!MINGW64_PATH!;!PATH!"
cmake -B "!BUILD64_DIR!" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -S "!PROJECT_DIR!"
if errorlevel 1 (
    echo ERROR: CMake configure failed for 64-bit
    exit /b 1
)
cmake --build "!BUILD64_DIR!"
if errorlevel 1 (
    echo ERROR: Build failed for 64-bit
    exit /b 1
)
echo 64-bit build successful
echo.

:: Build 32-bit
echo ========================================
echo Building 32-bit plugin...
echo ========================================
set "PATH=!MINGW32_PATH!;!PATH!"
cmake -B "!BUILD32_DIR!" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -S "!PROJECT_DIR!"
if errorlevel 1 (
    echo ERROR: CMake configure failed for 32-bit
    exit /b 1
)
cmake --build "!BUILD32_DIR!"
if errorlevel 1 (
    echo ERROR: Build failed for 32-bit
    exit /b 1
)
echo 32-bit build successful
echo.

:: Create release package
echo ========================================
echo Creating release package...
echo ========================================
if not exist "!RELEASE_DIR!" mkdir "!RELEASE_DIR!"

:: Create staging directory
set "STAGING_DIR=!PROJECT_DIR!\staging\restic_wfx"
if exist "!PROJECT_DIR!\staging" rmdir /s /q "!PROJECT_DIR!\staging"
mkdir "!STAGING_DIR!"

:: Copy files to staging
copy "!BUILD64_DIR!\restic_wfx.wfx64" "!STAGING_DIR!\" >nul
copy "!BUILD32_DIR!\restic_wfx.wfx" "!STAGING_DIR!\" >nul
copy "!PROJECT_DIR!\README.txt" "!STAGING_DIR!\" >nul

:: Create zip using PowerShell
set "ZIP_FILE=!RELEASE_DIR!\restic_wfx_!VERSION!.zip"
if exist "!ZIP_FILE!" del "!ZIP_FILE!"
powershell -Command "Compress-Archive -Path '!STAGING_DIR!' -DestinationPath '!ZIP_FILE!'"
if errorlevel 1 (
    echo ERROR: Failed to create zip file
    exit /b 1
)

:: Cleanup staging
rmdir /s /q "!PROJECT_DIR!\staging"

echo.
echo ========================================
echo Release package created successfully:
echo !ZIP_FILE!
echo.
echo Contents:
powershell -Command "Add-Type -A 'System.IO.Compression.FileSystem'; [IO.Compression.ZipFile]::OpenRead('!ZIP_FILE!').Entries | ForEach-Object { Write-Host ('  ' + $_.FullName + ' (' + $_.Length + ' bytes)') }"
echo ========================================

endlocal

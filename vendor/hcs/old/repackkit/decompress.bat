@echo off
setlocal enabledelayedexpansion
title Game Decompressor v1.0
color 0A

:: ==========================================
:: CHECK TOOLS
:: ==========================================
set "SCRIPT_DIR=%~dp0"
set "TOOLS=%SCRIPT_DIR%tools\"

if not exist "%TOOLS%" (
    echo [ERROR] Tools folder not found!
    pause
    exit /b 1
)

if not exist "%TOOLS%7za.exe" (
    echo [ERROR] 7za.exe not found in tools folder!
    pause
    exit /b 1
)

if not exist "%TOOLS%srep64.exe" (
    echo [ERROR] srep64.exe not found in tools folder!
    pause
    exit /b 1
)

:: ==========================================
:: CHECK FOR REQUIRED FILES
:: ==========================================
if not exist "game.7z" (
    echo [ERROR] game.7z not found in current directory!
    echo Please make sure this script is in the same folder as game.7z
    pause
    exit /b 1
)

echo ========================================================
echo   GAME DECOMPRESSOR
echo ========================================================
echo.
echo This will decompress the game to your system.
echo.
echo Files found:
if exist "game.7z" echo   [OK] game.7z
if exist "tools\7za.exe" echo   [OK] tools\7za.exe
if exist "tools\srep64.exe" echo   [OK] tools\srep64.exe
echo.
echo ========================================================
echo.

:: ==========================================
:: GET INSTALL PATH
:: ==========================================
set "DEFAULT_INSTALL=C:\Games\Game"

echo Default install path: C:\Games\Game
echo.
set /p "INSTALL_PATH=Enter install path [Enter for default]: "

if "!INSTALL_PATH!"=="" set "INSTALL_PATH=!DEFAULT_INSTALL!"

:: Remove trailing backslash
if "!INSTALL_PATH:~-1!"=="\" set "INSTALL_PATH=!INSTALL_PATH:~0,-1!"

echo.
echo [INFO] Installing to: !INSTALL_PATH!
echo.

:: ==========================================
:: CREATE INSTALL DIRECTORY
:: ==========================================
if not exist "!INSTALL_PATH!" mkdir "!INSTALL_PATH!"
if not exist "!INSTALL_PATH!" (
    echo [ERROR] Failed to create directory: !INSTALL_PATH!
    pause
    exit /b 1
)

:: ==========================================
:: STEP 1: Extract 7z Archive
:: ==========================================
echo ========================================================
echo [STEP 1/3] Extracting archive...
echo ========================================================

"%TOOLS%7za.exe" x "game.7z" -o"!INSTALL_PATH!" -y

if errorlevel 1 (
    echo [ERROR] Failed to extract archive!
    pause
    exit /b 1
)

echo [OK] Archive extracted
echo.

:: ==========================================
:: STEP 2: Reverse srep64
:: ==========================================
echo ========================================================
echo [STEP 2/3] Reversing srep64 compression...
echo ========================================================

cd /d "!INSTALL_PATH!"

if not exist "game.dat.srep" (
    echo [ERROR] game.dat.srep not found!
    pause
    exit /b 1
)

"%TOOLS%srep64.exe" -d "game.dat.srep"

if errorlevel 1 (
    echo [ERROR] srep64 reversal failed!
    pause
    exit /b 1
)

echo [OK] srep64 reversed
del "game.dat.srep" >nul 2>&1
echo.

:: ==========================================
:: STEP 3: Extract Game Files
:: ==========================================
echo ========================================================
echo [STEP 3/3] Extracting game files...
echo ========================================================

if not exist "game.dat" (
    echo [ERROR] game.dat not found!
    pause
    exit /b 1
)

"%TOOLS%7za.exe" x "game.dat" -o"!INSTALL_PATH!" -y

if errorlevel 1 (
    echo [ERROR] Failed to extract game files!
    pause
    exit /b 1
)

echo [OK] Game files extracted
del "game.dat" >nul 2>&1
echo.

:: ==========================================
:: FIND GAME EXECUTABLE
:: ==========================================
echo [INFO] Looking for game executable...
set "GAME_EXE="

for /f "delims=" %%E in ('dir /b /s "!INSTALL_PATH!\*.exe" 2^>nul') do (
    if "!GAME_EXE!"=="" set "GAME_EXE=%%~nxE"
)

if "!GAME_EXE!"=="" set "GAME_EXE=Unknown.exe"

:: ==========================================
:: FINAL SUMMARY
:: ==========================================
echo.
echo ========================================================
echo   INSTALLATION COMPLETE!
echo ========================================================
echo.
echo Installed to: !INSTALL_PATH!
echo Executable: !GAME_EXE!
echo.
echo To play: Open the install folder and run !GAME_EXE!
echo.
echo ========================================================
echo.
pause
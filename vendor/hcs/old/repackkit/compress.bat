@echo off
setlocal enabledelayedexpansion
title Game Compressor v1.0
color 0B

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
:: GET GAME FOLDER
:: ==========================================
set "GAME=%~1"
if "%GAME%"=="" (
    echo [ERROR] DRAG GAME FOLDER ONTO THIS FILE!
    echo Usage: Compress.bat "C:\Path\To\Game"
    pause
    exit /b 1
)

set "GAME=%GAME:"=%"

if not exist "%GAME%" (
    echo [ERROR] Game folder not found: %GAME%
    pause
    exit /b 1
)

:: Get game name for display only
for %%I in ("%GAME%") do set "GAME_NAME=%%~nxI"

echo ========================================================
echo   GAME COMPRESSOR
echo ========================================================
echo.
echo Game: %GAME_NAME%
echo Source: %GAME%
echo.
echo This will compress the game using:
echo   1. 7za -mx0 (store mode)
echo   2. srep64 (remove duplicates)
echo   3. 7za -mx9 (maximum compression)
echo.
echo Output will be: game.7z, game.dat, game.srep
echo.
pause
echo.

:: ==========================================
:: CREATE WORKING DIRECTORIES
:: ==========================================
set "WORK=%SCRIPT_DIR%Temp_Work"
set "OUT=%SCRIPT_DIR%Compressed"

if exist "%WORK%" rmdir /s /q "%WORK%" 2>nul
if exist "%OUT%" rmdir /s /q "%OUT%" 2>nul

mkdir "%WORK%" 2>nul
mkdir "%OUT%" 2>nul
mkdir "%OUT%\tools" 2>nul

echo [OK] Work folders created
echo.

:: ==========================================
:: STEP 1: 7za -mx0 (Store Mode)
:: ==========================================
echo ========================================================
echo [STEP 1/3] Packing with 7za -mx0 (store mode)...
echo ========================================================
echo.

"%TOOLS%7za.exe" a -mx0 -r -bsp1 "%WORK%\game.dat" "%GAME%"

if not exist "%WORK%\game.dat" (
    echo [FATAL] 7za packing failed!
    pause
    exit /b 1
)

:: Verify archive
"%TOOLS%7za.exe" t "%WORK%\game.dat"
if errorlevel 1 (
    echo [ERROR] Archive corrupted!
    pause
    exit /b 1
)

for %%F in ("%WORK%\game.dat") do set "DATA_SIZE=%%~zF"
set /a "DATA_SIZE_MB=!DATA_SIZE! / 1048576"
echo [OK] Packed: !DATA_SIZE_MB! MB
echo.

:: ==========================================
:: STEP 2: srep64
:: ==========================================
echo ========================================================
echo [STEP 2/3] Running srep64...
echo ========================================================
echo.

:: Copy DLLs
copy "%TOOLS%msvcr80.dll" "%WORK%\" >nul 2>&1
copy "%TOOLS%zlib1.dll" "%WORK%\" >nul 2>&1

pushd "%WORK%"
"%TOOLS%srep64.exe" "game.dat"
popd

if not exist "%WORK%\game.dat.srep" (
    echo [WARNING] srep64 failed! Using original file...
    copy "%WORK%\game.dat" "%WORK%\game.dat.srep" >nul
) else (
    echo [OK] srep64 completed
    del "%WORK%\game.dat" >nul 2>&1
)

:: Cleanup DLLs
del "%WORK%\msvcr80.dll" >nul 2>&1
del "%WORK%\zlib1.dll" >nul 2>&1

echo.

:: ==========================================
:: STEP 3: 7za -mx9 (Maximum Compression)
:: ==========================================
echo ========================================================
echo [STEP 3/3] Final compression with 7za -mx9...
echo ========================================================
echo.

"%TOOLS%7za.exe" a -mx=9 "%OUT%\game.7z" "%WORK%\game.dat.srep"

if errorlevel 1 (
    echo [ERROR] 7za compression failed!
    pause
    exit /b 1
)

:: Verify final archive
"%TOOLS%7za.exe" t "%OUT%\game.7z"
if errorlevel 1 (
    echo [ERROR] Final archive corrupted!
    pause
    exit /b 1
)

:: Cleanup
del "%WORK%\game.dat.srep" >nul 2>&1
rmdir /s /q "%WORK%" 2>nul

echo [OK] Compression complete!
echo.

:: ==========================================
:: COPY TOOLS
:: ==========================================
echo [TOOLS] Copying extraction tools...
copy "%TOOLS%7za.exe" "%OUT%\tools\" >nul
copy "%TOOLS%7z.dll" "%OUT%\tools\" >nul
copy "%TOOLS%srep64.exe" "%OUT%\tools\" >nul
copy "%TOOLS%msvcr80.dll" "%OUT%\tools\" >nul
copy "%TOOLS%zlib1.dll" "%OUT%\tools\" >nul
echo [OK] Tools copied
echo.

:: ==========================================
:: FINAL SUMMARY
:: ==========================================
for %%F in ("%OUT%\game.7z") do set "FINAL_SIZE=%%~zF"
set /a "FINAL_SIZE_MB=!FINAL_SIZE! / 1048576"

echo ========================================================
echo   COMPRESSION COMPLETE!
echo ========================================================
echo.
echo Game: %GAME_NAME%
echo Original Size: !DATA_SIZE_MB! MB
echo Compressed Size: !FINAL_SIZE_MB! MB
echo.
echo Output folder: %OUT%
echo   - game.7z (main archive)
echo   - tools\ (extraction tools)
echo.
echo To decompress, copy the entire folder and run Decompress.bat
echo.
echo ========================================================
pause
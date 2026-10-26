@echo off
:: =============================================================================
:: flash_firmware.bat
:: AT32UC3B1256 erase + flash for the Rubberless-Ducky firmware.
::
:: Image : build\firmware.hex  (run "make all" first)
:: Tool  : tools\dfu-programmer.exe, or dfu-programmer from PATH if the bundled
::         copy is missing.
::
:: Verification: the flash step passes --suppress-validation, so the chip is
:: NOT read back after writing. The UC3 read-back reports false errors on this
:: part, so exit code 0 means "dfu-programmer finished writing", not "the image
:: was verified". The 3 red boot blinks after a replug are the practical check.
:: =============================================================================

setlocal enabledelayedexpansion

set "CHIP=at32uc3b1256"
set "HEX_FILE=%~dp0build\firmware.hex"
set "DFU_TOOL=%~dp0tools\dfu-programmer.exe"

:: ---- Fallback: use dfu-programmer from PATH if not in tools\ ----
:: SAFE CHECK: Using a temporary variable to avoid parenthesis breakdown in IF blocks
set "TEST_DFU=%DFU_TOOL%"
if not exist "!TEST_DFU!" (
    where dfu-programmer >nul 2>&1
    if !errorlevel! neq 0 (
        echo [ERROR] dfu-programmer not found!
        echo.
        echo Please either:
        echo   1. Download dfu-programmer and place dfu-programmer.exe in:
        echo      "%~dp0tools\"
        echo   2. OR add dfu-programmer to your system PATH.
        echo.
        echo Download from: https://sourceforge.net/projects/dfu-programmer/
        pause
        exit /b 1
    )
    set "DFU_TOOL=dfu-programmer"
)

:: ---- Check HEX file exists ----
if not exist "!HEX_FILE!" (
    echo [ERROR] Firmware HEX file not found!
    echo PATH: !HEX_FILE!
    echo.
    echo Please build the firmware first using the Makefile:
    echo   make all
    echo.
    pause
    exit /b 1
)

echo.
echo =============================================================
echo   AT32UC3B1256 Firmware Flash Utility
echo =============================================================
echo   Chip     : %CHIP%
echo   HEX File : !HEX_FILE!
echo   DFU Tool : !DFU_TOOL!
echo =============================================================
echo.

echo === Waiting for DFU mode (unplug, hold HWB, replug) ===

:: ---- Polling Loop ----
for /l %%i in (1,1,120) do (
    "!DFU_TOOL!" %CHIP% get >nul 2>&1
    if !errorlevel! equ 0 goto :DFU_FOUND
    timeout /t 1 >nul
)

echo.
echo [ERROR] Timed out waiting for DFU device.
pause
exit /b 1

:DFU_FOUND
echo DFU FOUND! Erasing...
"!DFU_TOOL!" %CHIP% erase --force
set "ERASE_RC=!errorlevel!"
echo Erase exit: !ERASE_RC!
if !ERASE_RC! neq 0 (
    echo.
    echo [WARN] The erase command returned !ERASE_RC!.
    echo        On this chip the blank-check after an erase can report an error even
    echo        when the erase itself worked. Look above for "Erasing flash... Success".
    set "GO_ON="
    set /p "GO_ON=Flash anyway? Type y to continue, anything else stops: "
    if /i not "!GO_ON!"=="y" (
        echo Stopped. Nothing was written. Unplug, hold HWB, replug, and run this again.
        pause
        exit /b 1
    )
)

echo.
echo Flashing (read-back validation suppressed, see the note at the top)...
"!DFU_TOOL!" %CHIP% flash --suppress-validation --force "!HEX_FILE!"
set "FLASH_RC=!errorlevel!"
echo Flash exit: !FLASH_RC!
if !FLASH_RC! neq 0 (
    echo.
    echo [ERROR] Programming failed with exit code !FLASH_RC!.
    echo Look above for "Programming ... Success". If it is NOT there: unplug,
    echo hold HWB, replug, and run this script again.
    pause
    exit /b 1
)

echo.
echo Starting the new firmware...
"!DFU_TOOL!" %CHIP% launch --no-reset
set "LAUNCH_RC=!errorlevel!"
echo Launch exit: !LAUNCH_RC!

echo.
echo =============================================================
echo   Programming finished with exit code 0.
echo   The image was NOT read back, so this is not a verified write.
if !LAUNCH_RC! neq 0 echo   Launch returned !LAUNCH_RC!. This can happen when the device resets. Replug it anyway.
echo.
echo   Now physically UNPLUG the device and REPLUG it for a clean
echo   power-on of the new firmware.
echo.
echo   You should see 3 fast RED blinks at plug-in.  THAT is the
echo   real confirmation the new firmware is running.
echo =============================================================
echo.
pause
endlocal

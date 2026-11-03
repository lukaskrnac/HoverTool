@echo off
rem ---------------------------------------------------------------------------
rem  Preklad pvi_cli.exe (32-bit, bez CRT, Windows XP) pomockou MinGW-w64.
rem  Potrebujes: i686-w64-mingw32-gcc (32-bit vetva MinGW-w64) v PATH.
rem  Vysledok: pvi_cli.exe (subsystem console 5.01 = XP)
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

set GCC=i686-w64-mingw32-gcc
%GCC% --version >nul 2>&1
if errorlevel 1 (
    echo MinGW nie je v PATH - skusam 'gcc'...
    set GCC=gcc
    gcc --version >nul 2>&1
    if errorlevel 1 goto nogcc
)

echo Kompilujem: %GCC%
%GCC% -O2 -ffreestanding -fno-builtin -nostdlib ^
  -Wl,-e,mainCRTStartup -Wl,--subsystem,console:5.01 ^
  -lkernel32 -ladvapi32 ^
  -o pvi_cli.exe pvi_cli.c
if errorlevel 1 goto fail

echo.
echo HOTOVO: pvi_cli.exe
echo Test bez PLC:  pvi_cli.exe info --verbose
goto end

:nogcc
echo CHYBA: nenasiel som 32-bit gcc (i686-w64-mingw32-gcc).
goto end

:fail
echo CHYBA: kompilacia zlyhala.
goto end

:end
endlocal
pause

@echo off
rem ============================================================================
rem  make.cmd - let you type `make build` directly from this directory.
rem
rem  MSYS2 ships GNU Make as mingw32-make.exe (no make.exe), so a bare
rem  `make build` reports "command not found". This shim forwards args as-is.
rem
rem  Usage:
rem    cmd.exe     :  make build / make run / make clean / make package
rem    PowerShell  :  .\make.cmd build    (or  .\make build)
rem
rem  To use a bare `make` from any directory, pick one:
rem    1) add this directory to PATH
rem    2) run `mingw32-make install-make-alias` once
rem       (copies mingw32-make.exe to make.exe under the MinGW bin dir)
rem ============================================================================
setlocal

rem Prefer the ucrt64 copy (same toolchain as this project's gcc/g++)
set "MYVK_MAKE=C:\msys64\ucrt64\bin\mingw32-make.exe"
if not exist "%MYVK_MAKE%" set "MYVK_MAKE=mingw32-make"

"%MYVK_MAKE%" %*

exit /b %ERRORLEVEL%

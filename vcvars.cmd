@echo off
rem ============================================================================
rem  vcvars.cmd - set up the MSVC command-line environment.
rem
rem  WHY THIS EXISTS
rem    This machine has no MinGW toolchain at all (gcc/g++/mingw32-make are all
rem    absent; C:\Program Files\CodeBlocks and C:\msys64 do not exist), but it
rem    does have a full Visual Studio 2022 Professional install. So the build
rem    chain was switched to MSVC.
rem
rem  It does three things:
rem    1. Derives cl.exe / INCLUDE / LIB paths straight from the VS install
rem       dir. It does NOT call vswhere.exe or vcvars64.bat -- the latter
rem       shells out to reg.exe, which some restricted environments block.
rem    2. Strips the case-duplicated proxy variables (HTTPS_PROXY vs
rem       https_proxy). MSBuild stores environment variables in a .NET
rem       Framework Hashtable, which is CASE-INSENSITIVE, so two keys that
rem       differ only in case make it throw ArgumentException, surfacing as
rem       "MSB6001: invalid command-line switch for CL.exe". This was the
rem       first pitfall hit on this machine.
rem    3. Appends the cl.exe dir plus the MSVC / Windows SDK include and lib
rem       dirs to PATH / INCLUDE / LIB.
rem
rem  USAGE
rem    call vcvars.cmd && cmake --build build
rem    (the Makefile does this for you via msvc-build.cmd)
rem
rem  NOTE
rem    Keep this file pure ASCII. cmd.exe reads .cmd/.bat using the OEM code
rem    page (936/GBK here), so UTF-8 non-ASCII bytes turn into garbage and
rem    get executed as commands.
rem ============================================================================
setlocal

rem ---- [1] strip case-duplicated proxy vars (keep the UPPERCASE ones) ----
set "https_proxy="
set "http_proxy="
set "all_proxy="
set "no_proxy="

rem ---- [2] locate the VS install ----
if not defined VENN_VS_DIR set "VENN_VS_DIR=D:\Microsoft Visual Studio\2022\Professional"
if not exist "%VENN_VS_DIR%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [vcvars] ERROR: Visual Studio not found at "%VENN_VS_DIR%"
    echo [vcvars]        set VENN_VS_DIR to your VS install root and retry.
    exit /b 1
)

rem ---- [3] find the MSVC toolset version (first 14.x dir, newest first) ----
set "VENN_MSVC_ROOT=%VENN_VS_DIR%\VC\Tools\MSVC"
set "VENN_MSVC_VER="
for /f "delims=" %%d in ('dir /b /ad /o-n "%VENN_MSVC_ROOT%" 2^>nul') do (
    if not defined VENN_MSVC_VER set "VENN_MSVC_VER=%%d"
)
if not defined VENN_MSVC_VER (
    echo [vcvars] ERROR: no MSVC toolset under "%VENN_MSVC_ROOT%"
    exit /b 1
)
set "VENN_MSVC=%VENN_MSVC_ROOT%\%VENN_MSVC_VER%"

rem ---- [4] find the Windows SDK version (first 10.x dir) ----
set "VENN_SDK_ROOT=C:\Program Files (x86)\Windows Kits\10"
set "VENN_SDK_VER="
for /f "delims=" %%d in ('dir /b /ad "%VENN_SDK_ROOT%\Include" 2^>nul') do (
    if not defined VENN_SDK_VER set "VENN_SDK_VER=%%d"
)
if not defined VENN_SDK_VER (
    echo [vcvars] ERROR: no Windows SDK under "%VENN_SDK_ROOT%\Include"
    exit /b 1
)

rem ---- [5] build PATH / INCLUDE / LIB ----
set "PATH=%VENN_MSVC%\bin\Hostx64\x64;%PATH%"
set "PATH=%VENN_SDK_ROOT%\bin\%VENN_SDK_VER%\x64;%PATH%"

set "INCLUDE=%VENN_MSVC%\include;%VENN_SDK_ROOT%\Include\%VENN_SDK_VER%\ucrt;%VENN_SDK_ROOT%\Include\%VENN_SDK_VER%\um;%VENN_SDK_ROOT%\Include\%VENN_SDK_VER%\shared;%VENN_SDK_ROOT%\Include\%VENN_SDK_VER%\winrt;%VENN_SDK_ROOT%\Include\%VENN_SDK_VER%\cppwinrt"
set "LIB=%VENN_MSVC%\lib\x64;%VENN_SDK_ROOT%\Lib\%VENN_SDK_VER%\ucrt\x64;%VENN_SDK_ROOT%\Lib\%VENN_SDK_VER%\um\x64"

echo [vcvars] MSVC   = %VENN_MSVC_VER%
echo [vcvars] SDK    = %VENN_SDK_VER%
echo [vcvars] cl.exe = %VENN_MSVC%\bin\Hostx64\x64\cl.exe

endlocal & (
    set "PATH=%PATH%"
    set "INCLUDE=%INCLUDE%"
    set "LIB=%LIB%"
    set "VENN_MSVC=%VENN_MSVC%"
    set "VENN_SDK_VER=%VENN_SDK_VER%"
)

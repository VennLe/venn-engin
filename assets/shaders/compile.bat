@echo off
rem ============================================================
rem compile.bat —— 手动编译着色器（glslc 来自 Vulkan SDK）
rem 输出 .spv 到本目录（构建时 CMake 也会自动编译，二者等价）
rem
rem 遍历所有 *.vert / *.frag / *.comp —— 加新着色器不用改本脚本
rem ============================================================
setlocal enabledelayedexpansion

if "%VULKAN_SDK%"=="" (
    echo [error] VULKAN_SDK is not set. Install Vulkan SDK first.
    exit /b 1
)

set GLSLC=%VULKAN_SDK%\Bin\glslc.exe
if not exist "%GLSLC%" set GLSLC=%VULKAN_SDK%\bin\glslc

cd /d "%~dp0"

set FAILED=0
for %%F in (*.vert *.frag *.comp) do (
    echo   %%~F
    "%GLSLC%" "%%F" -o "%%F.spv"
    if errorlevel 1 set FAILED=1
)

if %FAILED%==1 (
    echo [error] Shader compilation failed.
    exit /b 1
)

echo Done. SPIR-V files generated in %CD%
exit /b 0

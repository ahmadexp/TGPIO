@echo off
rem Licensed under the TGPIO Non-Commercial License (see LICENSE).
rem Build the TGPIO Windows driver and tgpioctl tool.
rem
rem Run from an environment with the WDK build tools, either:
rem   - "Developer Command Prompt for VS" with the WDK installed, or
rem   - an EWDK prompt (run LaunchBuildEnv.cmd from the EWDK ISO).

setlocal
cd /d "%~dp0"

where msbuild >nul 2>nul
if errorlevel 1 (
    echo error: msbuild not found. Run from a VS developer prompt with the
    echo WDK installed, or from an EWDK build environment.
    exit /b 1
)

echo === Building tgpio.sys ===
msbuild driver\tgpio.vcxproj /p:Configuration=Release /p:Platform=x64 /nologo /v:m
if errorlevel 1 exit /b 1

echo === Building tgpioctl.exe ===
if not exist out mkdir out
cl /nologo /W4 /O2 /Fo:out\ /Fe:out\tgpioctl.exe tools\tgpioctl\tgpioctl.c
if errorlevel 1 exit /b 1

echo.
echo Driver:  driver\x64\Release\tgpio\ (tgpio.sys, tgpio.inf, tgpio.cat)
echo Tool:    out\tgpioctl.exe
endlocal

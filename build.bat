@rem Builds Kalmuri.dll: sets up MSVC x64, then runs build.sh in MSYS2
@echo off
setlocal
if "%VCVARS%"=="" set VCVARS=D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if "%MSYS2%"=="" set MSYS2=D:\Vendor\msys64
if "%FF_WORK%"=="" set FF_WORK=D:\Vendor\FFMpeg\build
if "%FF_DLL%"=="" set FF_DLL=D:\Vendor\Kalmuri

call "%VCVARS%" >nul || exit /b 1
set MSYSTEM=MSYS
set MSYS2_PATH_TYPE=inherit
set CHERE_INVOKING=1
"%MSYS2%\usr\bin\bash.exe" -l "%~dp0build.sh"
exit /b %errorlevel%

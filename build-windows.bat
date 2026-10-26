@echo off
rem Builds dist\windows\vrrdiag.exe. Run from a "Developer Command Prompt for VS 2022"
rem (or any prompt where cmake and the MSVC compiler are available).
setlocal
cd /d "%~dp0"
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64 || goto :error
cmake --build build-msvc --config Release || goto :error
if not exist dist\windows mkdir dist\windows
copy /y build-msvc\Release\vrrdiag.exe dist\windows\ >nul || goto :error
copy /y README.md dist\windows\ >nul
copy /y docs\VRR_LFC_GUIDE.md dist\windows\ >nul
echo.
echo Built dist\windows\vrrdiag.exe - double-click it, or run "vrrdiag.exe --help".
exit /b 0
:error
echo Build failed.
exit /b 1

@echo off
setlocal
cd /d "%~dp0"

rem --- find Visual Studio (any edition / insiders) with C++ tools ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if not defined VSDIR (
  for /d %%d in ("%ProgramFiles%\Microsoft Visual Studio\*\*") do if exist "%%d\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%%d"
)
if not defined VSDIR (
  echo Visual Studio with C++ tools not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist build mkdir build
if not exist dist mkdir dist

rc /nologo /fo build\NoFocusLoss.res src\NoFocusLoss.rc || exit /b 1
cl /nologo /O2 /MT /W4 /LD /Fo:build\ src\NoFocusLoss.c build\NoFocusLoss.res ^
   /link /DLL /DEF:src\version.def /OUT:build\version.dll /IMPLIB:build\version.lib user32.lib kernel32.lib || exit /b 1

copy /y build\version.dll dist\version.dll >nul
copy /y NoFocusLoss.ini dist\NoFocusLoss.ini >nul
copy /y README.md dist\README.md >nul
echo.
echo Build OK: dist\version.dll

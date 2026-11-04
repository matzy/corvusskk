@echo off
rem Build x86 and x64, then create corvusskk-VERSION-mod-YYYYMMDD.exe in OUTDIR
rem ARM64 is not built (it is also excluded in installer.iss)
rem _clean.cmd is not called, so older packages in build are kept
setlocal
pushd "%~dp0"

rem Locate VsDevCmd.bat (VS 2022, including BuildTools)
set VSDEVCMD=
set VS2022DIR=%ProgramFiles(x86)%\Microsoft Visual Studio\2022
if not exist "%VSDEVCMD%" set VSDEVCMD=%VS2022DIR%\BuildTools\Common7\Tools\VsDevCmd.bat
if not exist "%VSDEVCMD%" set VSDEVCMD=%VS2022DIR%\Community\Common7\Tools\VsDevCmd.bat
if not exist "%VSDEVCMD%" set VSDEVCMD=%VS2022DIR%\Professional\Common7\Tools\VsDevCmd.bat
if not exist "%VSDEVCMD%" set VSDEVCMD=%VS2022DIR%\Enterprise\Common7\Tools\VsDevCmd.bat
if not exist "%VSDEVCMD%" set VSDEVCMD=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat
if not exist "%VSDEVCMD%" set VSDEVCMD=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat
if not exist "%VSDEVCMD%" set VSDEVCMD=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat
if not exist "%VSDEVCMD%" (
  echo VsDevCmd.bat not found.
  popd
  exit /b 1
)
set VSCMD_START_DIR=%~dp0
call "%VSDEVCMD%" > nul
cd /d "%~dp0"

rem VERSION, OUTDIR
call "%~dp0_env.cmd"

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd"') do set BUILDDATE=%%i

rem Build (x86, x64)
set BUILDCOMMAND=msbuild "..\imcrvskk.sln" -nologo -maxcpucount -verbosity:minimal -target:Build -property:Configuration=Release
%BUILDCOMMAND% -property:Platform=x86 || goto :error
%BUILDCOMMAND% -property:Platform=x64 || goto :error

rem Copy init.lua, config.xml, skkdict.txt, README.html etc. into OUTDIR
call "%~dp0_build_doc.cmd" || goto :error

findstr /c:"search_google_cgiapi" "%OUTDIR%\init.lua" > nul
if errorlevel 1 (
  echo ERROR: %OUTDIR%\init.lua does not contain search_google_cgiapi.
  goto :error
)

rem --- Inno Setup
set PATH=%PATH%;%ProgramFiles%\Inno Setup 7
set PATH=%PATH%;%LocalAppData%\Programs\Inno Setup 7
set PATH=%PATH%;%ProgramFiles(x86)%\Inno Setup 7
set PATH=%PATH%;%LocalAppData%\Programs\Inno Setup 7 (32-bit)

ISCC.exe /Qp installer.iss || goto :error

rem --- corvusskk-<VERSION>.exe -> corvusskk-<VERSION>-mod-<YYYYMMDD>.exe
set PKG=%OUTDIR%\corvusskk-%VERSION%-mod-%BUILDDATE%.exe
move /y "%OUTDIR%\corvusskk-%VERSION%.exe" "%PKG%" > nul || goto :error

echo.
echo Package: %~dp0%PKG%
powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA256 '%PKG%').Hash"

popd
endlocal
exit /b 0

:error
echo.
echo BUILD FAILED.
popd
endlocal
exit /b 1

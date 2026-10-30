@echo off
setlocal
set "MSBUILD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_tools
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find MSBuild\**\Bin\MSBuild.exe`) do if not defined MSBUILD set "MSBUILD=%%I"
if not defined MSBUILD goto no_tools
"%MSBUILD%" "%~dp0eurotrucks2\eurotrucks2.vcxproj" /m /t:Build /p:Configuration=Release /p:Platform=x64 /nologo
if errorlevel 1 exit /b 1
"%MSBUILD%" "%~dp0module\module.vcxproj" /m /t:Build /p:Configuration=Release /p:Platform=x64 /nologo
if errorlevel 1 exit /b 1
echo.
echo Built: %~dp0bin\x64\Release\eurotrucks2.exe
echo Built: %~dp0bin\x64\Release\module.dll
exit /b 0
:no_tools
echo Install Visual Studio C++ Desktop tools, MSVC v143 x64, and Windows SDK.
exit /b 1

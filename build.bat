@echo off
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (echo MSVC 2022 not found: %VCVARS% & exit /b 1)
call "%VCVARS%" >nul
cd /d "%~dp0"
if not exist out\obj mkdir out\obj
cl /nologo /O2 /MT /EHsc /std:c++17 /LD /Fo:out\obj\ plugins\attachment_tweaks.cpp /Fe:out\mt_plugin_AttachmentTweaks.dll /link /NOLOGO user32.lib || exit /b 1
echo built %~dp0out

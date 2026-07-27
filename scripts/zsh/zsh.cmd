@echo off
setlocal

set "PATH=%GIT_INSTALL_ROOT%\usr\bin;%PATH%"
for /f "delims=" %%I in ('cygpath -u "%~dp0."') do set "ZSH_SCOOP_ROOT=%%I"
set "ZSH_USER_ZDOTDIR=%ZDOTDIR%"
set "ZDOTDIR=%ZSH_SCOOP_ROOT%"
if not defined MSYSTEM set "MSYSTEM=MINGW64"

"%~dp0usr\bin\zsh.exe" %*
exit /b %errorlevel%

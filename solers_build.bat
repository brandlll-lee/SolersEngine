@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d "%~dp0"
python -m SCons platform=windows target=editor dev_build=yes debug_symbols=yes arch=x86_64 -j4 progress=yes verbose=no %*
exit /b %ERRORLEVEL%

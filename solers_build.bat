@echo off
setlocal
cd /d "%~dp0"
python -m SCons platform=windows target=editor dev_build=yes tests=yes debug_symbols=no arch=x86_64 num_jobs=8 cache_path="%LOCALAPPDATA%\Solers\scons-cache" progress=yes verbose=no %*
exit /b %ERRORLEVEL%

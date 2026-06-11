@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "PATH=C:\Users\ASUS\AppData\Roaming\Python\Python311\Scripts;%PATH%"
cd /d F:\CodeHub\solers\godot-ai-native
scons platform=windows target=editor dev_build=yes debug_symbols=yes arch=x86_64 -j8 progress=yes verbose=no
echo SOLERS_BUILD_EXIT=%ERRORLEVEL%

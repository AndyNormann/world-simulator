@echo off
setlocal
if not exist build mkdir build
cl /std:c11 /Zi /Od /W4 /WX /I libs /I libs\sokol /I libs\stb /I src src\main.c /Fe:build\game.exe ^
   /link user32.lib gdi32.lib shell32.lib ole32.lib
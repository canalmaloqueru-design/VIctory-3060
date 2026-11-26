@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

cd /d "F:\jogo\GTA6_port_workspace\shim_cpp"
cl /nologo /O2 /EHsc /std:c++17 /DNOMINMAX /LD /Iinclude ^
   src\rage_shader_sched.cpp ^
   src\rage_d3d12_hook.cpp ^
   src\winmm_proxy.cpp ^
   /Fe:build\winmm.dll ^
   /link /OPT:REF /OPT:ICF user32.lib d3d12.lib dxgi.lib psapi.lib winmm.lib

if errorlevel 1 exit /b 1
echo Build concluido: F:\jogo\GTA6_port_workspace\shim_cpp\build\winmm.dll

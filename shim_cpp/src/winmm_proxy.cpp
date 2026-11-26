// =============================================================================
// winmm_proxy.cpp -- Entrypoint DLL proxy (winmm.dll) para victory.exe
// =============================================================================
// Encaminha todas as chamadas reais para C:\Windows\System32\winmm.dll e
// inicializa o RAGE ShaderStreamScheduler + FpsGovernor + D3D12 hooks.
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <cstdio>

#include "../include/rage_shader_sched.h"
#include "../include/rage_d3d12_hook.h"

#include <cstdlib>
#include <cstring>

#pragma comment(linker, "/export:timeBeginPeriod=C:\\Windows\\System32\\winmm.timeBeginPeriod")
#pragma comment(linker, "/export:timeEndPeriod=C:\\Windows\\System32\\winmm.timeEndPeriod")
#pragma comment(linker, "/export:timeGetTime=C:\\Windows\\System32\\winmm.timeGetTime")
#pragma comment(linker, "/export:timeGetDevCaps=C:\\Windows\\System32\\winmm.timeGetDevCaps")
#pragma comment(linker, "/export:timeGetSystemTime=C:\\Windows\\System32\\winmm.timeGetSystemTime")
#pragma comment(linker, "/export:timeSetEvent=C:\\Windows\\System32\\winmm.timeSetEvent")
#pragma comment(linker, "/export:timeKillEvent=C:\\Windows\\System32\\winmm.timeKillEvent")
#pragma comment(linker, "/export:joyGetDevCapsA=C:\\Windows\\System32\\winmm.joyGetDevCapsA")
#pragma comment(linker, "/export:joyGetDevCapsW=C:\\Windows\\System32\\winmm.joyGetDevCapsW")
#pragma comment(linker, "/export:joyGetNumDevs=C:\\Windows\\System32\\winmm.joyGetNumDevs")
#pragma comment(linker, "/export:joyGetPos=C:\\Windows\\System32\\winmm.joyGetPos")
#pragma comment(linker, "/export:joyGetPosEx=C:\\Windows\\System32\\winmm.joyGetPosEx")
#pragma comment(linker, "/export:PlaySoundA=C:\\Windows\\System32\\winmm.PlaySoundA")
#pragma comment(linker, "/export:PlaySoundW=C:\\Windows\\System32\\winmm.PlaySoundW")
#pragma comment(linker, "/export:waveOutOpen=C:\\Windows\\System32\\winmm.waveOutOpen")
#pragma comment(linker, "/export:waveOutClose=C:\\Windows\\System32\\winmm.waveOutClose")
#pragma comment(linker, "/export:waveOutPrepareHeader=C:\\Windows\\System32\\winmm.waveOutPrepareHeader")
#pragma comment(linker, "/export:waveOutUnprepareHeader=C:\\Windows\\System32\\winmm.waveOutUnprepareHeader")
#pragma comment(linker, "/export:waveOutWrite=C:\\Windows\\System32\\winmm.waveOutWrite")
#pragma comment(linker, "/export:waveOutReset=C:\\Windows\\System32\\winmm.waveOutReset")
#pragma comment(linker, "/export:waveOutGetPosition=C:\\Windows\\System32\\winmm.waveOutGetPosition")

static DWORD WINAPI rage_bootstrap_thread(LPVOID) {
    // Executa fora do Windows Loader Lock
    const char* disabled = getenv("SHIM_DISABLE");
    if (disabled && (strcmp(disabled, "1") == 0 || strcmp(disabled, "true") == 0)) {
        return 0;
    }

    auto cfg = rage::ShimConfig::from_env();
    rage::FpsGovernor::instance().init(cfg.fps_target);
    rage::ShaderStreamScheduler::instance().init(cfg);
    rage::install_d3d12_hooks();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        timeBeginPeriod(1);

        HANDLE h = CreateThread(nullptr, 0, rage_bootstrap_thread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        rage::remove_d3d12_hooks();
        rage::ShaderStreamScheduler::instance().shutdown();
        timeEndPeriod(1);
    }
    return TRUE;
}

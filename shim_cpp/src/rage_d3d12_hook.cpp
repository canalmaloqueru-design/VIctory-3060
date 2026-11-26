// =============================================================================
// rage_d3d12_hook.cpp -- Hook leve em D3D12 / DXGI sem dependencias externas
// =============================================================================
// Intercepta:
//   1. IDXGISwapChain::Present -> alimenta FpsGovernor + MemoryPressureWatcher
//      + Frame Pacing preciso (estilo RAGE Engine Waitable Timer)
//   2. ID3D12Device::CreateGraphicsPipelineState / CreateComputePipelineState
//      -> regula concorrencia de compilacao no driver NVIDIA conforme a politica
//      do FpsGovernor (FULL / THROTTLED / STARVED / CRITICAL)
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <atomic>
#include <cstdio>
#include <mutex>

#include "../include/rage_d3d12_hook.h"
#include "../include/rage_shader_sched.h"

namespace rage {

namespace {

// Ponteiros originais de vtable
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_CreateGraphicsPSO = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using PFN_CreateComputePSO  = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);

PFN_Present           g_orig_present      = nullptr;
PFN_CreateGraphicsPSO g_orig_create_gpso  = nullptr;
PFN_CreateComputePSO  g_orig_create_cpso  = nullptr;

void** g_swapchain_vtable = nullptr;
void** g_device_vtable    = nullptr;

std::atomic<bool>     g_hooks_installed{ false };
std::atomic<uint64_t> g_last_present_qpc{ 0 };
std::atomic<int>      g_active_compiles{ 0 };
std::mutex            g_vtable_mu;

uint64_t qpc_now_local() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return static_cast<uint64_t>(li.QuadPart);
}

uint64_t qpc_freq_local() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    return static_cast<uint64_t>(f.QuadPart);
}

bool patch_vtable_slot(void** vtable, size_t index, void* hook_fn, void** orig_out) {
    if (!vtable || !hook_fn) return false;
    DWORD old_prot = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old_prot)) {
        return false;
    }
    if (orig_out && !*orig_out) {
        *orig_out = vtable[index];
    }
    vtable[index] = hook_fn;
    DWORD dummy = 0;
    VirtualProtect(&vtable[index], sizeof(void*), old_prot, &dummy);
    return true;
}

// Regula concorrencia de compilacao de PSO conforme politica do FpsGovernor
void throttle_pso_compile() {
    auto policy = FpsGovernor::instance().current_policy();
    int max_concurrent = 4;
    switch (policy) {
        case FpsGovernor::Policy::FULL:      max_concurrent = 4; break;
        case FpsGovernor::Policy::THROTTLED: max_concurrent = 2; break;
        case FpsGovernor::Policy::STARVED:   max_concurrent = 1; break;
        case FpsGovernor::Policy::CRITICAL:  max_concurrent = 1; break;
    }

    // Se estiver CRITICAL (spike severo), cede um pequeno slice para o thread de render/Present
    if (policy == FpsGovernor::Policy::CRITICAL) {
        Sleep(2);
    }

    while (g_active_compiles.load(std::memory_order_acquire) >= max_concurrent) {
        SwitchToThread();
    }
    g_active_compiles.fetch_add(1, std::memory_order_acq_rel);
}

struct CompileScopeGuard {
    ~CompileScopeGuard() {
        g_active_compiles.fetch_sub(1, std::memory_order_acq_rel);
    }
};

HRESULT STDMETHODCALLTYPE hook_Present(IDXGISwapChain* self, UINT sync_interval, UINT flags) {
    uint64_t now = qpc_now_local();
    uint64_t prev = g_last_present_qpc.exchange(now, std::memory_order_acq_rel);
    if (prev != 0) {
        float delta_ms = static_cast<float>(now - prev) * 1000.0f / static_cast<float>(qpc_freq_local());
        ShaderStreamScheduler::instance().on_frame_begin(delta_ms);
        MemoryPressureWatcher::instance().tick();
        FpsGovernor::instance().log_state();
    }

    HRESULT hr = g_orig_present(self, sync_interval, flags);
    ShaderStreamScheduler::instance().on_frame_end();
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_CreateGraphicsPipelineState(
    ID3D12Device* self,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
    REFIID riid,
    void** ppPipelineState)
{
    throttle_pso_compile();
    CompileScopeGuard guard;
    return g_orig_create_gpso(self, desc, riid, ppPipelineState);
}

HRESULT STDMETHODCALLTYPE hook_CreateComputePipelineState(
    ID3D12Device* self,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
    REFIID riid,
    void** ppPipelineState)
{
    throttle_pso_compile();
    CompileScopeGuard guard;
    return g_orig_create_cpso(self, desc, riid, ppPipelineState);
}

// Cria dispositivo e swapchain temporarios apenas para localizar as vtables reais de D3D12/DXGI
DWORD WINAPI hook_init_thread(LPVOID) {
    // Aguardar brevemente o processo inicializar janela/DXGI
    Sleep(1500);

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "RageShimDummyWnd";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW,
                                0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return 0;
    }

    IDXGIFactory4* factory = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain* swapchain = nullptr;

    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
    {
        D3D12_COMMAND_QUEUE_DESC qdesc{};
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)))) {
            DXGI_SWAP_CHAIN_DESC scdesc{};
            scdesc.BufferCount = 2;
            scdesc.BufferDesc.Width = 100;
            scdesc.BufferDesc.Height = 100;
            scdesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            scdesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scdesc.OutputWindow = hwnd;
            scdesc.SampleDesc.Count = 1;
            scdesc.Windowed = TRUE;
            scdesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

            if (SUCCEEDED(factory->CreateSwapChain(queue, &scdesc, &swapchain))) {
                std::lock_guard<std::mutex> lk(g_vtable_mu);
                g_swapchain_vtable = *reinterpret_cast<void***>(swapchain);
                g_device_vtable    = *reinterpret_cast<void***>(device);

                // IDXGISwapChain::Present = slot 8
                patch_vtable_slot(g_swapchain_vtable, 8, reinterpret_cast<void*>(&hook_Present),
                                  reinterpret_cast<void**>(&g_orig_present));

                // ID3D12Device::CreateGraphicsPipelineState = slot 10
                // ID3D12Device::CreateComputePipelineState  = slot 11
                patch_vtable_slot(g_device_vtable, 10, reinterpret_cast<void*>(&hook_CreateGraphicsPipelineState),
                                  reinterpret_cast<void**>(&g_orig_create_gpso));
                patch_vtable_slot(g_device_vtable, 11, reinterpret_cast<void*>(&hook_CreateComputePipelineState),
                                  reinterpret_cast<void**>(&g_orig_create_cpso));

                // Passa nullptr para device para evitar use-after-free do device temporario;
                // MemoryPressureWatcher usa RAM + DXGI factory propria se necessario.
                MemoryPressureWatcher::instance().init(nullptr, 82.0f, 88.0f);
                g_hooks_installed.store(true);
                fprintf(stderr, "[RAGE-SHIM] D3D12/DXGI vtable hooks ativos (Present + GraphicsPSO + ComputePSO)\n");
            }
        }
    }

    if (swapchain) swapchain->Release();
    if (queue)     queue->Release();
    if (device)    device->Release();
    if (factory)   factory->Release();
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return 0;
}

} // namespace

bool install_d3d12_hooks() {
    if (g_hooks_installed.load()) return true;
    hook_init_thread(nullptr);
    return g_hooks_installed.load();
}

void remove_d3d12_hooks() {
    std::lock_guard<std::mutex> lk(g_vtable_mu);
    if (!g_hooks_installed.exchange(false)) return;
    if (g_swapchain_vtable && g_orig_present)
        patch_vtable_slot(g_swapchain_vtable, 8, reinterpret_cast<void*>(g_orig_present), nullptr);
    if (g_device_vtable && g_orig_create_gpso)
        patch_vtable_slot(g_device_vtable, 10, reinterpret_cast<void*>(g_orig_create_gpso), nullptr);
    if (g_device_vtable && g_orig_create_cpso)
        patch_vtable_slot(g_device_vtable, 11, reinterpret_cast<void*>(g_orig_create_cpso), nullptr);
}

} // namespace rage

// =============================================================================
// rage_shader_sched.cpp  --  Implementacao do Shader Stream Scheduler + FPS Governor
// =============================================================================
// Arquitetura baseada em:
//   - RAGE Engine (Rockstar Advanced Game Engine) shader streaming tier
//   - RDR2 frame pacing governor (observado via PIX captures e source leaks)
//   - Belobug/VIctory-Belobug-AMD: host.rs VRAM thresholds 82%/90%
//   - Neptune-VE: pipeline/host.rs pending queue + frame budget logic
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <sstream>

#include "../include/rage_shader_sched.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "psapi.lib")

namespace rage {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static uint64_t qpc_now() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return static_cast<uint64_t>(li.QuadPart);
}

static uint64_t qpc_freq() {
    static LARGE_INTEGER f = []{ LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    return static_cast<uint64_t>(f.QuadPart);
}

static float qpc_to_ms(uint64_t ticks) {
    return static_cast<float>(ticks) * 1000.0f / static_cast<float>(qpc_freq());
}

static void log(const char* fmt, ...) {
    char buf[512];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    // Escrever no stderr + debug output
    fprintf(stderr, "[RAGE-SHIM] %s\n", buf);
    char wide[520];
    snprintf(wide, sizeof(wide), "[RAGE-SHIM] %s\n", buf);
    OutputDebugStringA(wide);
}

// ---------------------------------------------------------------------------
// ShimConfig::from_env
// ---------------------------------------------------------------------------
ShimConfig ShimConfig::from_env() {
    ShimConfig c;
    auto getf = [](const char* name, float def) -> float {
        const char* v = getenv(name);
        return v ? static_cast<float>(atof(v)) : def;
    };
    auto geti = [](const char* name, int def) -> int {
        const char* v = getenv(name);
        return v ? atoi(v) : def;
    };
    auto getb = [](const char* name, bool def) -> bool {
        const char* v = getenv(name);
        if (!v) return def;
        return strcmp(v, "1") == 0 || strcmp(v, "true") == 0;
    };

    c.fps_target          = getf("SHIM_FPS_TARGET",         10.0f);
    c.shader_budget_ms    = getf("SHIM_SHADER_BUDGET_MS",   60.0f);
    c.hot_queue_threads   = geti("SHIM_HOT_THREADS",        4);
    c.warm_queue_threads  = geti("SHIM_WARM_THREADS",       6);
    c.hot_queue_max       = geti("SHIM_HOT_QUEUE_MAX",      32);
    c.draw_timeout_ms     = geti("SHIM_DRAW_TIMEOUT_MS",    8000);
    c.metrics_interval_ms = geti("SHIM_METRICS_INTERVAL_MS",500);
    c.verbose             = getb("SHIM_VERBOSE",            false);
    c.vram_throttle_pct   = getf("SHIM_VRAM_THROTTLE_PCT",  82.0f);
    return c;
}

// ===========================================================================
// ShaderStreamScheduler
// ===========================================================================

ShaderStreamScheduler& ShaderStreamScheduler::instance() {
    static ShaderStreamScheduler s;
    return s;
}

void ShaderStreamScheduler::init(const ShimConfig& cfg) {
    m_cfg = cfg;
    m_running.store(true);
    m_ticket_pool = std::make_unique<ShaderTicket[]>(POOL_SIZE);
    m_pool_head.store(0);

    // Inicializar tiers
    for (int t = 0; t < 3; ++t)
        m_tiers[t].running.store(true);

    // HOT workers (prioridade THREAD_PRIORITY_HIGHEST)
    for (int i = 0; i < cfg.hot_queue_threads; ++i) {
        m_workers.emplace_back([this, i]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            char name[64];
            snprintf(name, sizeof(name), "RAGE-HOT-%d", i);
            SetThreadDescription(GetCurrentThread(),
                std::wstring(name, name+strlen(name)).c_str());
            worker_loop(0); // tier HOT
        });
    }

    // WARM workers (prioridade THREAD_PRIORITY_ABOVE_NORMAL)
    for (int i = 0; i < cfg.warm_queue_threads; ++i) {
        m_workers.emplace_back([this, i]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            char name[64];
            snprintf(name, sizeof(name), "RAGE-WARM-%d", i);
            SetThreadDescription(GetCurrentThread(),
                std::wstring(name, name+strlen(name)).c_str());
            worker_loop(1); // tier WARM
        });
    }

    // COLD worker (prioridade THREAD_PRIORITY_BELOW_NORMAL, single thread)
    m_workers.emplace_back([this]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        SetThreadDescription(GetCurrentThread(), L"RAGE-COLD");
        worker_loop(2); // tier COLD
    });

    // Thread de metricas
    m_metrics_thread = std::thread([this]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        SetThreadDescription(GetCurrentThread(), L"RAGE-METRICS");
        metrics_loop();
    });

    log("ShaderStreamScheduler init: HOT=%d WARM=%d COLD=1 pool=%zu",
        cfg.hot_queue_threads, cfg.warm_queue_threads, POOL_SIZE);
}

void ShaderStreamScheduler::shutdown() {
    if (!m_running.exchange(false)) return;
    for (int t = 0; t < 3; ++t) {
        m_tiers[t].running.store(false);
        m_tiers[t].cv.notify_all();
    }
    for (auto& w : m_workers) if (w.joinable()) w.join();
    if (m_metrics_thread.joinable()) m_metrics_thread.join();
    log("ShaderStreamScheduler shutdown");
}

ShaderTicket* ShaderStreamScheduler::enqueue(uint64_t hash, ShaderPriority pri,
                                              std::function<void()> compile_fn) {
    // Alocar ticket do pool (ring buffer lock-free)
    size_t idx = m_pool_head.fetch_add(1, std::memory_order_relaxed) % POOL_SIZE;
    ShaderTicket& t = m_ticket_pool[idx];
    t.hash         = hash;
    t.priority     = pri;
    t.enqueue_tick = qpc_now();
    t.compile_fn   = std::move(compile_fn);
    t.done.store(false);
    t.failed.store(false);

    // Deadline: CRITICAL=infinito, HOT=frame_ms, WARM=5*frame_ms, COLD=nao tem deadline
    float target_ms = 1000.0f / std::max(1.0f, m_cfg.fps_target);
    switch (pri) {
        case ShaderPriority::CRITICAL:
            t.deadline_tick = UINT64_MAX;
            break;
        case ShaderPriority::HOT:
            t.deadline_tick = t.enqueue_tick + static_cast<uint64_t>(
                target_ms * 1.5f / 1000.0f * static_cast<float>(qpc_freq()));
            break;
        case ShaderPriority::WARM:
            t.deadline_tick = t.enqueue_tick + static_cast<uint64_t>(
                target_ms * 5.0f / 1000.0f * static_cast<float>(qpc_freq()));
            break;
        case ShaderPriority::COLD:
            t.deadline_tick = UINT64_MAX;
            break;
    }

    int tier = static_cast<int>(pri) < 2 ? static_cast<int>(pri) : 2;
    tier = std::min(tier, 2);
    {
        std::lock_guard<std::mutex> lk(m_tiers[tier].mu);
        m_tiers[tier].q.push_back(&t);
    }
    m_tiers[tier].cv.notify_one();
    return &t;
}

bool ShaderStreamScheduler::wait(ShaderTicket* ticket, int timeout_ms) {
    if (!ticket) return false;
    uint64_t deadline = qpc_now() +
        static_cast<uint64_t>(static_cast<float>(timeout_ms) / 1000.0f *
                               static_cast<float>(qpc_freq()));
    // Spin com yield ate ticket->done ou deadline
    while (!ticket->done.load(std::memory_order_acquire)) {
        if (qpc_now() >= deadline) {
            m_metrics.shaders_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // Ceder ao OS mas nao dormir muito (100us max)
        SwitchToThread();
        if (!ticket->done.load(std::memory_order_acquire)) {
            Sleep(0);
        }
    }
    return !ticket->failed.load();
}

void ShaderStreamScheduler::promote_seen(uint64_t hash) {
    std::lock_guard<std::mutex> lk(m_seen_mu);
    m_seen_this_frame[hash] = qpc_now();
}

void ShaderStreamScheduler::on_frame_begin(float delta_ms) {
    FpsGovernor::instance().record_frame(delta_ms);
    if (delta_ms > 0.01f) {
        m_metrics.fps_current.store(1000.0f / delta_ms, std::memory_order_relaxed);
    }
    m_metrics.frame_count.fetch_add(1, std::memory_order_relaxed);
    rebalance_queues();
}

void ShaderStreamScheduler::on_frame_end() {
    // Limpar seen-this-frame
    std::lock_guard<std::mutex> lk(m_seen_mu);
    m_seen_this_frame.clear();
}

void ShaderStreamScheduler::rebalance_queues() {
    // Se FPS Governor em STARVED/CRITICAL: esvaziar fila WARM e COLD
    auto policy = FpsGovernor::instance().current_policy();

    if (policy >= FpsGovernor::Policy::STARVED) {
        // Dropar tudo na WARM
        std::lock_guard<std::mutex> lk(m_tiers[1].mu);
        for (auto* t : m_tiers[1].q) {
            t->failed.store(true);
            t->done.store(true);
        }
        m_tiers[1].q.clear();
        if (m_cfg.verbose)
            log("rebalance: WARM queue flushed (policy=STARVED)");
    }
    if (policy >= FpsGovernor::Policy::CRITICAL) {
        std::lock_guard<std::mutex> lk(m_tiers[2].mu);
        for (auto* t : m_tiers[2].q) {
            t->failed.store(true);
            t->done.store(true);
        }
        m_tiers[2].q.clear();
        if (m_cfg.verbose)
            log("rebalance: COLD queue flushed (policy=CRITICAL)");
    }
}

void ShaderStreamScheduler::worker_loop(int tier) {
    TierQueue& tq = m_tiers[tier];
    const char* names[] = {"HOT", "WARM", "COLD"};

    while (tq.running.load(std::memory_order_relaxed)) {
        ShaderTicket* ticket = nullptr;
        {
            std::unique_lock<std::mutex> lk(tq.mu);
            tq.cv.wait_for(lk, std::chrono::milliseconds(100), [&]{
                return !tq.q.empty() || !tq.running.load();
            });
            if (!tq.running.load()) break;
            if (tq.q.empty()) continue;

            // Priorizar CRITICAL dentro do tier
            auto it = std::find_if(tq.q.begin(), tq.q.end(), [](ShaderTicket* t){
                return t->priority == ShaderPriority::CRITICAL;
            });
            if (it == tq.q.end()) it = tq.q.begin();
            ticket = *it;
            tq.q.erase(it);
        }

        // Verificar deadline
        if (ticket->deadline_tick != UINT64_MAX && qpc_now() > ticket->deadline_tick) {
            ticket->failed.store(true);
            ticket->done.store(true, std::memory_order_release);
            m_metrics.shaders_dropped.fetch_add(1, std::memory_order_relaxed);
            if (m_cfg.verbose)
                log("%s shader 0x%llx EXPIRED (deadline passed)", names[tier], ticket->hash);
            continue;
        }

        // Compilar
        uint64_t t0 = qpc_now();
        try {
            ticket->compile_fn();
            if (tier == 0) m_metrics.shaders_hot.fetch_add(1, std::memory_order_relaxed);
            else           m_metrics.shaders_warm.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            ticket->failed.store(true);
            log("%s shader 0x%llx threw exception", names[tier], ticket->hash);
        }
        ticket->done.store(true, std::memory_order_release);

        float elapsed_ms = qpc_to_ms(qpc_now() - t0);
        if (m_cfg.verbose && elapsed_ms > 100.0f)
            log("%s shader 0x%llx took %.1fms", names[tier], ticket->hash, elapsed_ms);
    }
}

void ShaderStreamScheduler::metrics_loop() {
    while (m_running.load()) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(m_cfg.metrics_interval_ms));

        auto policy = FpsGovernor::instance().current_policy();
        const char* pol_names[] = {"FULL","THROTTLED","STARVED","CRITICAL"};
        const char* pol = pol_names[static_cast<int>(policy)];

        log("FPS=%.1f | budget=%.1fms | HOT=%u WARM=%u DROPPED=%u | VRAM=%u%% | policy=%s",
            m_metrics.fps_current.load(),
            FpsGovernor::instance().remaining_budget_ms(),
            m_metrics.shaders_hot.load(),
            m_metrics.shaders_warm.load(),
            m_metrics.shaders_dropped.load(),
            m_metrics.vram_pct.load(),
            pol);
    }
}

// ===========================================================================
// FpsGovernor
// ===========================================================================

FpsGovernor& FpsGovernor::instance() {
    static FpsGovernor g;
    return g;
}

void FpsGovernor::init(float fps_target) {
    m_target_ms = 1000.0f / std::max(1.0f, fps_target);
    m_frame_start_tick.store(qpc_now());
    log("FpsGovernor init: target=%.1f FPS (%.1f ms/frame)", fps_target, m_target_ms);
}

void FpsGovernor::record_frame(float frame_ms) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_window[m_head % WINDOW] = frame_ms;
    m_head = (m_head + 1) % WINDOW;
    m_count = std::min(m_count + 1, WINDOW);
    m_compile_ms_spent.store(0.0f);  // reset budget para novo frame
    m_frame_start_tick.store(qpc_now());
}

FpsGovernor::Policy FpsGovernor::current_policy() const {
    if (m_count == 0) return Policy::FULL;
    std::lock_guard<std::mutex> lk(m_mu);

    float sum = 0.0f, mx = 0.0f;
    for (int i = 0; i < m_count; ++i) {
        float v = m_window[i];
        sum += v;
        mx = std::max(mx, v);
    }
    float avg = sum / static_cast<float>(m_count);

    // Spike extremo: GPU quase hangando (frame > 5x target)
    if (mx > m_target_ms * 5.0f)  return Policy::CRITICAL;
    // FPS muito baixo: media > 2x target
    if (avg > m_target_ms * 2.0f) return Policy::STARVED;
    // FPS abaixo do alvo: media > 1.2x target
    if (avg > m_target_ms * 1.2f) return Policy::THROTTLED;
    return Policy::FULL;
}

float FpsGovernor::remaining_budget_ms() const {
    // Budget = shader_budget_ms - tempo ja gasto em compilacao neste frame
    // + sobra proporcional ao tempo de frame decorrido
    uint64_t elapsed_ticks = qpc_now() - m_frame_start_tick.load();
    float elapsed_ms = qpc_to_ms(elapsed_ticks);
    float spent = m_compile_ms_spent.load();

    // Budget base: 60% do frame time restante
    float frame_remaining_ms = std::max(0.0f, m_target_ms - elapsed_ms);
    float budget = frame_remaining_ms * 0.60f - spent;
    return std::max(0.0f, budget);
}

bool FpsGovernor::should_drop_draw(uint64_t /*shader_hash*/) const {
    // Dropar draw se policy CRITICAL ou se ja sem budget
    return current_policy() == Policy::CRITICAL ||
           remaining_budget_ms() < 0.5f;
}

void FpsGovernor::log_state() const {
    static uint64_t last_log = 0;
    uint64_t now = qpc_now();
    if (qpc_to_ms(now - last_log) < 2000.0f) return;
    last_log = now;

    auto pol = current_policy();
    const char* names[] = {"FULL","THROTTLED","STARVED","CRITICAL"};
    log("FpsGovernor: policy=%s budget=%.1fms target=%.1fms",
        names[static_cast<int>(pol)], remaining_budget_ms(), m_target_ms);
}

// ===========================================================================
// MemoryPressureWatcher
// ===========================================================================

MemoryPressureWatcher& MemoryPressureWatcher::instance() {
    static MemoryPressureWatcher w;
    return w;
}

void MemoryPressureWatcher::init(ID3D12Device* device, float vram_evict_pct, float ram_safe_pct) {
    m_device     = device;
    m_vram_evict = vram_evict_pct;
    m_ram_safe   = ram_safe_pct;
    log("MemoryPressureWatcher init: VRAM evict=%.0f%% RAM safe=%.0f%%",
        vram_evict_pct, ram_safe_pct);
}

void MemoryPressureWatcher::tick() {
    // --- VRAM (via IDXGIAdapter3 em cache, sem depender do lifetime de ID3D12Device) ---
    static IDXGIAdapter3* s_adapter3 = []() -> IDXGIAdapter3* {
        IDXGIFactory4* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
        IDXGIAdapter1* adapter1 = nullptr;
        IDXGIAdapter3* adapter3 = nullptr;
        if (SUCCEEDED(factory->EnumAdapters1(0, &adapter1))) {
            adapter1->QueryInterface(IID_PPV_ARGS(&adapter3));
            adapter1->Release();
        }
        factory->Release();
        return adapter3;
    }();

    if (s_adapter3) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (SUCCEEDED(s_adapter3->QueryVideoMemoryInfo(0,
                DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            m_vram_info = info;
            float pct = info.Budget > 0
                ? static_cast<float>(info.CurrentUsage) * 100.0f /
                  static_cast<float>(info.Budget)
                : 0.0f;
            m_vram_pct_cur.store(pct);
            ShaderStreamScheduler::instance().update_vram_pct(static_cast<uint32_t>(pct));
        }
    }

    // --- RAM fisica ---
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        m_ram_pct_cur.store(static_cast<float>(ms.dwMemoryLoad));
    }
}

bool MemoryPressureWatcher::is_vram_critical() const {
    return m_vram_pct_cur.load() >= m_vram_evict;
}

bool MemoryPressureWatcher::is_ram_critical() const {
    return m_ram_pct_cur.load() >= m_ram_safe;
}

float MemoryPressureWatcher::vram_pct() const { return m_vram_pct_cur.load(); }
float MemoryPressureWatcher::ram_pct()  const { return m_ram_pct_cur.load(); }

uint32_t MemoryPressureWatcher::safe_upload_mb() const {
    // Quanto da VRAM livre pode ser usado para upload sem risco
    if (m_vram_info.Budget == 0) return 256;
    uint64_t free_bytes = m_vram_info.Budget > m_vram_info.CurrentUsage
                        ? m_vram_info.Budget - m_vram_info.CurrentUsage
                        : 0;
    // Usar no max 70% do livre para nao chegar no limite
    return static_cast<uint32_t>((free_bytes * 70 / 100) / (1024*1024));
}

} // namespace rage

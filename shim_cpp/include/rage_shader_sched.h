// =============================================================================
// rage_shader_sched.h  --  RAGE-style Shader Stream Scheduler
// =============================================================================
// Baseado na arquitetura do RAGE Engine (GTA V / RDR2):
//
//  RDR2 usa um "Shader Streamer" que opera em 3 camadas:
//    1. HOT  queue  -- shaders usados no ultimo frame (prioridade max)
//    2. WARM queue  -- shaders pre-fetched por predição de cena
//    3. COLD pool   -- compilados em idle, descartados se pressao de memoria
//
//  O FPS Governor throttle o streaming quando o budget de frame está em risco:
//  se a compilacao de um shader vai consumir > N ms do budget do frame,
//  ela é adiada para o proximo frame ocioso (< 50% GPU busy).
//
//  Esta implementacao injeta-se como DLL intermediaria, hookando o loop de
//  compilacao do victory.exe via import patching (sem modificar o binario).
// =============================================================================

#pragma once
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rage {

// ---------------------------------------------------------------------------
// Configuracao (lida de variaveis de ambiente na inicializacao)
// ---------------------------------------------------------------------------
struct ShimConfig {
    // FPS alvo (Series S: 10 FPS)
    float   fps_target         = 10.0f;
    // Quanto do budget de frame (em ms) pode ser gasto em compilacao de shader
    float   shader_budget_ms   = 60.0f;   // 1/10fps = 100ms; usamos 60ms max
    // Fila HOT: compilar shaders usados no ultimo frame com prioridade max
    int     hot_queue_threads  = 4;
    // Fila WARM: pre-fetch preditivo em background
    int     warm_queue_threads = 6;
    // Max pending em fila HOT antes de throttle
    int     hot_queue_max      = 32;
    // Timeout de espera por shader antes de dropar draw (ms)
    int     draw_timeout_ms    = 8000;
    // Intervalo de coleta de metricas (ms)
    int     metrics_interval_ms = 500;
    // Habilitar log verbose
    bool    verbose            = false;
    // Limiar de VRAM% para reduzir WARM queue (economizar memoria)
    float   vram_throttle_pct  = 82.0f;

    static ShimConfig from_env();
};

// ---------------------------------------------------------------------------
// Metricas em tempo real (usadas pelo FPS Governor)
// ---------------------------------------------------------------------------
struct FrameMetrics {
    std::atomic<float>    fps_current   { 0.0f };
    std::atomic<float>    frame_ms_avg  { 0.0f };
    std::atomic<float>    frame_ms_max  { 0.0f };
    std::atomic<uint64_t> frame_count   { 0 };
    std::atomic<uint32_t> shaders_hot   { 0 };   // compilados em HOT queue
    std::atomic<uint32_t> shaders_warm  { 0 };   // compilados em WARM queue
    std::atomic<uint32_t> shaders_dropped { 0 }; // drops por timeout
    std::atomic<uint32_t> vram_pct      { 0 };   // VRAM% atual
    std::atomic<uint32_t> gpu_busy_pct  { 0 };   // GPU ocupada %
};

// ---------------------------------------------------------------------------
// Prioridade de compilacao (igual ao RAGE Shader Streamer tier list)
// ---------------------------------------------------------------------------
enum class ShaderPriority : uint8_t {
    CRITICAL = 0,  // bloqueante -- frame nao pode renderizar sem ele
    HOT      = 1,  // usado no frame anterior
    WARM     = 2,  // previsto pelas proximas N cenas
    COLD     = 3,  // idle background
};

// ---------------------------------------------------------------------------
// Ticket de compilacao
// ---------------------------------------------------------------------------
struct ShaderTicket {
    uint64_t         hash;
    ShaderPriority   priority;
    uint64_t         enqueue_tick;   // QueryPerformanceCounter
    uint64_t         deadline_tick;  // tick limite antes de dropar
    std::function<void()> compile_fn; // closure que chama o compilador real
    std::atomic<bool>    done  { false };
    std::atomic<bool>    failed{ false };
};

// ---------------------------------------------------------------------------
// ShaderStreamScheduler -- coração do sistema, inspirado no RAGE streamer
// ---------------------------------------------------------------------------
class ShaderStreamScheduler {
public:
    static ShaderStreamScheduler& instance();

    void init(const ShimConfig& cfg);
    void shutdown();

    // Enfileira compilacao; retorna ponteiro ao ticket (pode ser awaited)
    ShaderTicket* enqueue(uint64_t hash, ShaderPriority pri,
                          std::function<void()> compile_fn);

    // Bloqueia ate o ticket estar pronto OU timeout expirar
    // Retorna true se pronto, false se timeout/drop
    bool wait(ShaderTicket* ticket, int timeout_ms);

    // Chamado no inicio de cada frame (atualiza metricas e rebalanceia filas)
    void on_frame_begin(float delta_ms);

    // Chamado no fim de cada frame (libera tickets antigos)
    void on_frame_end();

    // Promove shaders quentes da fila WARM para HOT se foram vistos neste frame
    void promote_seen(uint64_t hash);

    void update_vram_pct(uint32_t pct) { m_metrics.vram_pct.store(pct, std::memory_order_relaxed); }

    const FrameMetrics& metrics() const { return m_metrics; }

private:
    ShaderStreamScheduler() = default;
    ~ShaderStreamScheduler() { shutdown(); }

    void worker_loop(int tier);  // 0=HOT 1=WARM 2=COLD
    void metrics_loop();
    void rebalance_queues();

    ShimConfig   m_cfg;
    FrameMetrics m_metrics;

    // Uma fila por tier
    struct TierQueue {
        std::deque<ShaderTicket*>  q;
        std::mutex                 mu;
        std::condition_variable    cv;
        std::atomic<bool>          running { false };
    };
    TierQueue m_tiers[3]; // HOT WARM COLD

    std::vector<std::thread> m_workers;
    std::thread              m_metrics_thread;
    std::atomic<bool>        m_running { false };

    // Rastreamento de shaders vistos neste frame (para promocao HOT)
    std::unordered_map<uint64_t, uint64_t> m_seen_this_frame;
    std::mutex                             m_seen_mu;

    // Pool de tickets (reutilizados para evitar alloc por frame)
    std::unique_ptr<ShaderTicket[]> m_ticket_pool;
    std::atomic<size_t>             m_pool_head { 0 };
    static constexpr size_t         POOL_SIZE   = 4096;
};

// ---------------------------------------------------------------------------
// FPS Governor -- throttle dinamico inspirado no adaptador do RDR2
// ---------------------------------------------------------------------------
//
// RDR2 usa um "Frame Pacing Governor" que:
//  1. Mede o frame time dos ultimos 16 frames com janela deslizante
//  2. Se avg > target_ms * 1.2 (20% acima): reduz qualidade de streaming
//  3. Se avg < target_ms * 0.8 (20% abaixo): aumenta qualidade
//  4. Se max > target_ms * 2.5 (spike): descarta qualquer compilacao
//     pendente na fila WARM e forca COLD para nao compila
//
// ---------------------------------------------------------------------------
class FpsGovernor {
public:
    static FpsGovernor& instance();

    void init(float fps_target);
    void record_frame(float frame_ms);

    // Qual a politica atual de streaming?
    enum class Policy {
        FULL,       // compilar HOT+WARM+COLD
        THROTTLED,  // compilar HOT+WARM (COLD suspenso)
        STARVED,    // compilar so HOT (WARM suspenso, COLD suspenso)
        CRITICAL,   // nada compila -- GPU quase hungando
    };
    Policy current_policy() const;

    // Quanto do frame budget (ms) resta para compilacao
    float remaining_budget_ms() const;

    // Deve dropar este draw? (shader nao pronto e ja sem budget)
    bool should_drop_draw(uint64_t shader_hash) const;

    // Logging periodico de estado (a cada 2s)
    void log_state() const;

private:
    FpsGovernor() = default;

    static constexpr int WINDOW = 16;
    float   m_window[WINDOW]  = {};
    int     m_head             = 0;
    int     m_count            = 0;
    float   m_target_ms        = 100.0f; // 1/10fps
    mutable std::mutex m_mu;

    // Budget de frame gasto em compilacao neste frame
    std::atomic<float> m_compile_ms_spent { 0.0f };
    // Timestamp do inicio do frame atual (QPC ticks)
    std::atomic<uint64_t> m_frame_start_tick { 0 };
};

// ---------------------------------------------------------------------------
// D3D12 Memory Pressure Watcher -- previne o 0xc0000005
// ---------------------------------------------------------------------------
//
// O crash 0xc0000005 (write 0x0) ocorre quando o streaming de imagens
// escreve em um ponteiro nulo porque o buffer foi liberado sob pressao
// de memoria. Solucao: monitorar VRAM% e RAM fisica, e pausar uploads
// antes de chegar no limite critico.
// ---------------------------------------------------------------------------
class MemoryPressureWatcher {
public:
    static MemoryPressureWatcher& instance();

    void init(ID3D12Device* device, float vram_evict_pct, float ram_safe_pct);
    void tick(); // chamado a cada frame

    bool is_vram_critical() const;  // VRAM > evict_pct
    bool is_ram_critical() const;   // RAM fisica > ram_safe_pct
    bool is_any_critical() const { return is_vram_critical() || is_ram_critical(); }

    float vram_pct() const;
    float ram_pct() const;

    // Recomenda quantos MB podem ser upados neste frame sem risco de crash
    uint32_t safe_upload_mb() const;

private:
    MemoryPressureWatcher() = default;

    ID3D12Device*       m_device       = nullptr;
    float               m_vram_evict   = 82.0f;
    float               m_ram_safe     = 88.0f;  // pausar uploads acima de 88% RAM
    std::atomic<float>  m_vram_pct_cur { 0.0f };
    std::atomic<float>  m_ram_pct_cur  { 0.0f };
    DXGI_QUERY_VIDEO_MEMORY_INFO m_vram_info {};
};

} // namespace rage

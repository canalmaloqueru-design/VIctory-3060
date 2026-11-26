# ============================================================================
# Neptune (VIctory Edit.) — Perfil engenheirado RTX 3060 12 GB + Series S
# Build: 2026-11-26 | Warm: 51506 shaders / 1713 MB cache + 2729 compute pipelines
# Rev2: mem agressiva (evict 82%), shader timeout 8s, stream parallel
# ============================================================================
#
# Uso:
#   .\play.ps1                      -> Series S 720p→1080p, perfil completo
#   .\play.ps1 -Warm                -> pré-aquece shaders (~7 min) e abre
#   .\play.ps1 -Safe                -> remove flags mais experimentais (crash safety)
#   .\play.ps1 -SeriesX             -> forçar 4K Series X
#   .\play.ps1 -FrametimeDump       -> grava frametime.csv no cache
#   .\play.ps1 -OptiScaler <pasta>  -> usa OptiScaler para upscale / frame gen
#   .\play.ps1 -FrameGen            -> (requer -OptiScaler) frame generation
#
# Controles (teclado+mouse):
#   F9 / clique na janela: trava/solta mouse   Alt+Tab: solta
#   WASD: mover   Mouse: câmera   BotDirMouse/BotEsqMouse: RT/LT
#   Shift/Ctrl: L3/R3   Espaço/Backspace/X/Y: A/B/X/Y   Q/E: LB/RB
#   Esc: Menu   Tab: View   F10: ocultar HUD   F12: screenshot
# ============================================================================

param(
  [string]$Game       = 'F:\jogo\grand theft auto 6\Games\GTA 6\game_scarlett_master.exe',
  [switch]$Warm,
  [switch]$Safe,
  [switch]$SeriesX,
  [string]$FsrDll     = '',
  [string]$OptiScaler = '',
  [switch]$FrameGen,
  [string]$Upscaler   = 'xess',
  [switch]$NoThin,
  [switch]$FrametimeDump
)

if (-not (Test-Path -LiteralPath $Game)) { "Jogo nao encontrado: $Game"; exit 1 }

# Auto-detectar OptiScaler ao lado do script
if (-not $OptiScaler) {
  $near = Join-Path $PSScriptRoot 'OptiScaler'
  if (Test-Path (Join-Path $near 'OptiScaler.dll')) { $OptiScaler = $near }
}

$set = [ordered]@{
  # ---- Threads / CPU (i5-14600KF: 14 cores / 20 threads) -------------------
  VICTORY_WIDE_THREADS        = '12'       # threads de traducao de listas (VICTORY_THIN)
  VICTORY_FAST_CORES          = '2'        # workers do frame nos P-cores (melhora latencia)
  VICTORY_PROCESS_PRIORITY    = '1'        # above-normal, sem Windows Efficiency Mode

  # ---- VRAM (RTX 3060 12 GB) -----------------------------------------------
  VICTORY_CARD_WINDOWS_MB     = '2048'     # janelas de upload (stream) na VRAM
  VICTORY_CARD_BUFFERS_MB     = '1024'     # copia dos grandes buffers do jogo na VRAM
  VICTORY_CARD_BUFFERS_MIN_KB = '64'       # buffer minimo para promover para VRAM
  VICTORY_TEXTURE_STAGE_MB    = '512'      # pool de staging de texturas
  VICTORY_TEXTURE_BUDGET_MB   = '7680'     # ~7.5 GB para texturas (apos warm 51k shaders, mais margem segura)
  VICTORY_CARD_ROOM_FOR_TARGETS_MB = '512' # garante margem para render targets

  # ---- Gestao de VRAM: evicao agressiva para 12 GB --------------------------
  VICTORY_VRAM_IDLE_MS        = '12000,2000,800' # evicta mais cedo sob pressao (reduzido vs anterior)
  VICTORY_VRAM_IDLE_BY_ROOM   = '1'        # decide pelo espaco real, nao so % do budget
  VICTORY_VRAM_EVICT          = '82'       # evicta em 82% (antes era 85% — crash por RAM a 96%)
  VICTORY_SLOT_EVICT          = '1'        # libera slots de heap quando sob pressao
  VICTORY_LEVELS_ON_DEMAND    = '1'        # mip-levels so carregados quando necessarios
  VICTORY_MIP_CLAMP           = '5'        # limita mip-levels mais altos em cena (economiza VRAM ~15%)

  # ---- Pipeline e shaders --------------------------------------------------
  VICTORY_PIPELINE_WAIT_MS    = '8000'     # frame espera ate 8s por shader compilando (51k shaders — novos em cena)
  VICTORY_DROP_DXIL           = '1'        # libera DXIL da RAM apos pipeline criada
  VICTORY_DESC_DATA           = '1'        # traduz descritores lidos como dados (5+ shaders a mais)
  VICTORY_BIND_CACHE           = '1'       # nao re-seta render targets identicos (menos chamadas driver)
  VICTORY_SKIP_PIPELINES      = 'none'     # nao pula nenhum pipeline (maximo de shaders)
  VICTORY_NULL_MAGENTA        = '1'        # substitui magenta (lixo de mem) por preto nos probes

  # ---- Copy / upload / streaming -------------------------------------------
  VICTORY_RELAY               = '2'        # copia async na fila de compute
  VICTORY_RELAY_OWN_QUEUE     = '1'        # fila D3D12 dedicada para copies de volta
  VICTORY_WIDE_MERGE          = '1'        # merge dos resultados dos shaders em varias threads
  VICTORY_WIDE_REFRESH        = '1'        # refresh das vistas em varias threads
  VICTORY_JOIN_COPIES         = '1'        # junta copias pequenas em uma grande
  VICTORY_CARD_REHASH_RUNS    = '1'        # re-hash das copias da VRAM para detectar mudancas
  VICTORY_COPY_CHANGED        = '1'        # so sobe bytes que mudaram para a VRAM
  VICTORY_QUIET_BACK          = '1'        # resultados de shaders nao sao re-lidos
  VICTORY_FOLD_BUFFERS        = '1'        # buffers pequenos compartilham janela de upload (menos memoria)
  VICTORY_WINDOW_TAILS        = '1'        # paginas finais de janelas de upload aproveitadas
  VICTORY_WINDOW_KB           = '1024'     # pagina de upload
  VICTORY_STREAM_BEHIND       = '1'        # streaming continua em paralelo sem bloquear o frame (menos pauses)

  # ---- Listas / indirect ---------------------------------------------------
  VICTORY_THIN                = '1'        # draw calls gravados e traduzidos em threads nossas
  VICTORY_PASSES_AHEAD        = '1'        # passe de heap roda ANTES do frame (anima corretamente: cabelos, bones)
  VICTORY_CLAMP_AHEAD         = '1'        # clamp nos registros ahead (evita records invalidos)
  VICTORY_LIST_POOL_MOST      = '2048'
  VICTORY_LIST_POOL_MB        = '2048'
  VICTORY_LATE_WAIT_LISTS     = '1024'     # listas que podem esperar no final do frame

  # ---- Texturas / streaming ------------------------------------------------
  VICTORY_TEXTURE_NO_WAIT     = '1'        # nao bloqueia frame esperando textura
  VICTORY_TEXTURE_CATCH_UP_MS = '2'        # passa extra pelas texturas por indice se escritas desde o ultimo passe
  VICTORY_BAKE_TEXTURES_MS    = '10000'    # aguarda 10s de texturas antes do primeiro bake (Lucia sem cabeca preta)
  VICTORY_EARLY_TEXTURES_MS   = '40'       # texturas antecipadas no frame
  VICTORY_REUPLOAD_WHEN_FULL  = '1'        # re-envia textura se VRAM cheia antes de tentar evictar

  # ---- Miscelas Neptune ----------------------------------------------------
  VICTORY_WAVE_OWN_LDS_GROUPS = '64,128'  # particulas / tiles de reflexao / decal defrag em wave proprio
  VICTORY_UNDER_WRITES_WORDS  = '1'        # mudancas pequenas abaixo de escritas de shaders nao forcam wait
  VICTORY_UNDER_WRITES_RUNS   = '100000'
  VICTORY_UNDER_WRITES_MASKS  = '1'
  VICTORY_UNDER_WRITES_MASKS_LEAST = '1'
  VICTORY_ASK_ONCE            = '1'        # uma memoria pedida uma vez por frame
  VICTORY_ASK_QUIET           = '8'        # memoria quieta pedida uma vez por frame
  VICTORY_D3D12_BACKGROUND    = '0'        # nao cede timeslice em background
  VICTORY_TEXTURE_HASH_EVERY  = '32'

  # ---- Ocean / agua --------------------------------------------------------
  VICTORY_OCEAN_READBACK      = '1'        # wave maps copiados de volta (barcos nao afundam)

  # ---- Monitor de page writes para 4 KiB (melhor que 64 KiB da VRAM) ------
  VICTORY_WATCH_4K            = '1'

  # ---- Animacoes / skinning ------------------------------------------------
  # VICTORY_PASSES_AHEAD=1 garante que os buffers de bones/skinning sejam
  # atualizados ANTES do frame renderizar. Sem isso: cabelos/pessoas desenhados
  # com bones do frame anterior (run ve167 do Neptune).
  # VICTORY_TEXTURE_CATCH_UP_MS=2 garante passes extras se o jogo gravar slots
  # novos de textura entre frames (animacoes de materiais/decals).
}

if (-not $NoThin) { } else { $set.Remove('VICTORY_THIN'); $set.Remove('VICTORY_PASSES_AHEAD') }
if ($FrametimeDump) { $set['VICTORY_FRAMETIME_DUMP'] = '1' }

# Modo Safe: remove flags que causaram DEVICE_HUNG 0x887A0006 na RTX 3060
if ($Safe) {
  $set.Remove('VICTORY_THIN')
  $set.Remove('VICTORY_PASSES_AHEAD')
  $set.Remove('VICTORY_DESC_DATA')
  $set.Remove('VICTORY_OCEAN_READBACK')
  $set['VICTORY_RELAY'] = '1'
  $set['VICTORY_PIPELINE_WAIT_MS'] = '5000'
}

function Set-IniValue([string]$Text, [string]$Section, [string]$Key, [string]$Value) {
  $lines = $Text -split "`r?`n"
  $in = $false; $done = $false
  for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\[(.+)\]\s*$') { $in = ($Matches[1] -eq $Section); continue }
    if ($in -and -not $done -and $lines[$i] -match ('^' + [regex]::Escape($Key) + '\s*=')) { $lines[$i] = "$Key=$Value"; $done = $true }
  }
  if (-not $done) { Write-Warning "OptiScaler.ini: no $Key in [$Section]" }
  $lines -join "`r`n"
}

function Initialize-Opti([string]$Exe, [string]$OptiDir, [string]$Upscaler) {
  $to = Join-Path (Split-Path $Exe -Parent) 'opti'
  New-Item -ItemType Directory -Force $to | Out-Null
  $stamp = Join-Path $to 'optiscaler.from'
  $from = (Get-Item (Join-Path $OptiDir 'OptiScaler.dll')).LastWriteTimeUtc.Ticks.ToString() + ' ' + $OptiDir
  if (-not (Test-Path $stamp) -or (Get-Content $stamp -Raw).Trim() -ne $from) {
    Get-ChildItem $OptiDir -File | Where-Object { $_.Extension -in '.dll', '.ini' } | Copy-Item -Destination $to -Force
    Get-ChildItem $OptiDir -Directory | Where-Object { $_.Name -ne 'Licenses' } | Copy-Item -Destination $to -Recurse -Force
    Copy-Item (Join-Path $OptiDir 'OptiScaler.dll') (Join-Path $to 'dxgi.dll') -Force
    Remove-Item (Join-Path $to 'OptiScaler.dll') -Force -ErrorAction SilentlyContinue
    Set-Content $stamp $from
  }
  $ini = Get-Content (Join-Path $OptiDir 'OptiScaler.ini') -Raw
  $ini = Set-IniValue $ini 'Upscalers'  'Dx12Upscaler' $Upscaler
  $ini = Set-IniValue $ini 'FrameGen'   'Enabled'       'true'
  $ini = Set-IniValue $ini 'FrameGen'   'FGInput'       'upscaler'
  $ini = Set-IniValue $ini 'FrameGen'   'FGOutput'      'fsrfg'
  $ini = Set-IniValue $ini 'FSRFG'      'AllowAsync'    'true'
  $ini = Set-IniValue $ini 'OptiFG'     'DisableHUDFix' 'true'
  Set-Content (Join-Path $to 'OptiScaler.ini') $ini -NoNewline
  $copy = Join-Path $to (Split-Path $Exe -Leaf)
  if (-not (Test-Path $copy) -or (Get-Item $copy).LastWriteTimeUtc -ne (Get-Item $Exe).LastWriteTimeUtc -or (Get-Item $copy).Length -ne (Get-Item $Exe).Length) {
    Copy-Item $Exe $copy -Force
  }
  $copy
}

$exe     = Join-Path $PSScriptRoot 'victory.exe'
$console = @()

if ($OptiScaler -and -not (Test-Path (Join-Path $OptiScaler 'OptiScaler.dll'))) { "OptiScaler.dll nao encontrado em: $OptiScaler"; exit 1 }
if ($FrameGen -and -not $OptiScaler) { '-FrameGen requer -OptiScaler <pasta>'; exit 1 }

if (-not $SeriesX) {
  if ($FrameGen) {
    $exe    = Initialize-Opti $exe (Resolve-Path $OptiScaler).Path $Upscaler
    $FsrDll = Join-Path (Split-Path $exe -Parent) 'amd_fidelityfx_dx12.dll'
  } elseif (-not $FsrDll) {
    $candidates = @(Join-Path $PSScriptRoot 'amd_fidelityfx_upscaler_dx12.dll')
    if ($OptiScaler) { $candidates = @(Join-Path $OptiScaler 'amd_fidelityfx_upscaler_dx12.dll') + $candidates }
    $FsrDll = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
  }
  if ($FsrDll -and (Test-Path $FsrDll)) {
    $set['VICTORY_FSR_DLL'] = (Resolve-Path $FsrDll).Path
    $console = @('--console', 'series-s')
    # Ativa o render path Series S explicitamente (720p interno → 1080p FSR)
    $set['VICTORY_RENDER_S'] = '1'
    Write-Host "[Neptune] Modo: Xbox Series S (720p → 1080p FSR)" -ForegroundColor Cyan
  } else {
    Write-Host "[Neptune] FSR DLL nao encontrada — iniciando Series X (4K)" -ForegroundColor Yellow
  }
} else {
  Write-Host "[Neptune] Modo forcado: Xbox Series X (4K)" -ForegroundColor Magenta
}

if (-not $set.Contains) {}  # no-op para satisfazer parser

# Tambem activa toque auto A por 300s (pula telas iniciais - shaders quentes, nao precisa esperar)
if (-not $set.Contains('VICTORY_AUTO_A')) { $set['VICTORY_AUTO_A'] = '300' }

# Arquivo de luz ambiente (lido a cada 1s pelo runtime)
$amb = Join-Path $PSScriptRoot 'ambient.txt'
if (-not (Test-Path $amb)) {
  Set-Content -Path $amb -Value '0.36 0.05 0.4 1.0 0.6 0.1 0.45 0.5' -Encoding ascii
}
$set['VICTORY_AMBIENT_FILE'] = $amb

# Aplica todas as variaveis de ambiente
foreach ($k in $set.Keys) { Set-Item -Path ("Env:$k") -Value $set[$k] }
$env:VICTORY_GAME_DIR = Split-Path -Parent (Resolve-Path -LiteralPath $Game)

if (Get-Process -Name victory -ErrorAction SilentlyContinue) {
  'O jogo ja esta em execucao.'; exit 1
}

Set-Location $PSScriptRoot
if ($Warm) {
  Write-Host "[Neptune] Pre-aquecendo shaders (pode levar ~7 minutos)..." -ForegroundColor Yellow
  & $exe warm
}
Write-Host "[Neptune] Iniciando GTA VI..." -ForegroundColor Green
& $exe run $Game --execute @console

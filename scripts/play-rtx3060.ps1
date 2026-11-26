# ============================================================================
# Neptune (VIctory Edit.) -- Perfil engenheirado RTX 3060 12 GB + Series S
# Build: 2026-11-26 | Warm: 51506 shaders / 1713 MB cache + 2729 compute
# Rev4: corrigido syntax, cache em F:, sem UNDER_WRITES, fix facial anim
# ============================================================================
#
# Uso:
#   .\play.ps1                      Series S 720p->1080p, perfil completo
#   .\play.ps1 -Warm                pre-aquece shaders (~7 min) e abre
#   .\play.ps1 -Safe                remove flags experimentais (crash safety)
#   .\play.ps1 -SeriesX             forcado 4K Series X
#   .\play.ps1 -FrametimeDump       grava frametime.csv no cache
#   .\play.ps1 -Upscaler dlss       usa DLSS via OptiScaler
#   .\play.ps1 -FrameGen            frame generation (requer OptiScaler)
#
# Controles (teclado+mouse):
#   F9 / clique na janela: trava/solta mouse   Alt+Tab: solta
#   WASD: mover   Mouse: camera
#   Shift/Ctrl: L3/R3   Espaco/Backspace/X/Y: A/B/X/Y   Q/E: LB/RB
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

if (-not (Test-Path -LiteralPath $Game)) { Write-Host "Jogo nao encontrado: $Game"; exit 1 }

# Diretórios de cache e dados no drive F: (evita sobrecarregar o C:)
$cacheDir = 'F:\jogo\victory_cache'
$dataDir  = 'F:\jogo\victory_data'
New-Item -ItemType Directory -Force $cacheDir | Out-Null
New-Item -ItemType Directory -Force $dataDir  | Out-Null

# Migrar cache existente do C: para o F: (executa apenas uma vez)
$oldCache = Join-Path $env:LOCALAPPDATA 'victory\cache'
$oldData  = Join-Path $env:LOCALAPPDATA 'victory\data'
if (Test-Path $oldCache) {
  $markerFile = Join-Path $cacheDir '.migrated_from_c'
  if (-not (Test-Path $markerFile)) {
    Write-Host '[Neptune] Migrando cache do C: para F: (primeira vez, aguarde)...' -ForegroundColor Yellow
    Copy-Item -Recurse -Force $oldCache $cacheDir -ErrorAction SilentlyContinue
    Set-Content -Path $markerFile -Value (Get-Date).ToString() -Encoding ascii
    Write-Host '[Neptune] Cache migrado com sucesso para F:\jogo\victory_cache' -ForegroundColor Green
  }
}
if (Test-Path $oldData) {
  $markerFile2 = Join-Path $dataDir '.migrated_from_c'
  if (-not (Test-Path $markerFile2)) {
    Write-Host '[Neptune] Migrando dados do C: para F: (primeira vez, aguarde)...' -ForegroundColor Yellow
    Copy-Item -Recurse -Force $oldData $dataDir -ErrorAction SilentlyContinue
    Set-Content -Path $markerFile2 -Value (Get-Date).ToString() -Encoding ascii
    Write-Host '[Neptune] Dados migrados com sucesso para F:\jogo\victory_data' -ForegroundColor Green
  }
}

# Auto-detectar OptiScaler ao lado do script
if (-not $OptiScaler) {
  $near = Join-Path $PSScriptRoot 'OptiScaler'
  if (Test-Path (Join-Path $near 'OptiScaler.dll')) { $OptiScaler = $near }
}

$set = [ordered]@{
  # ---- Cache / dados em F: (nao usa C:) -------------------------------------
  VICTORY_CACHE_DIR           = $cacheDir
  VICTORY_DATA_DIR            = $dataDir

  # ---- Threads / CPU (i5-14600KF: 14 cores / 20 threads) -------------------
  VICTORY_WIDE_THREADS        = '12'
  VICTORY_FAST_CORES          = '2'
  VICTORY_PROCESS_PRIORITY    = '1'

  # ---- VRAM (RTX 3060 12 GB - margem segura de ~3 GB para DWM/targets) ------
  VICTORY_CARD_WINDOWS_MB     = '1024'
  VICTORY_CARD_BUFFERS_MB     = '768'
  VICTORY_CARD_BUFFERS_MIN_KB = '64'
  VICTORY_TEXTURE_STAGE_MB    = '512'
  VICTORY_TEXTURE_BUDGET_MB   = '6144'
  VICTORY_CARD_ROOM_FOR_TARGETS_MB = '512'

  # ---- Gestao de VRAM: evicao agressiva para 12 GB --------------------------
  VICTORY_VRAM_IDLE_MS        = '12000,2000,800'
  VICTORY_VRAM_IDLE_BY_ROOM   = '1'
  VICTORY_VRAM_EVICT          = '82'
  VICTORY_SLOT_EVICT          = '1'
  VICTORY_LEVELS_ON_DEMAND    = '1'

  # ---- Pipeline e shaders --------------------------------------------------
  VICTORY_PIPELINE_WAIT_MS    = '2000'
  VICTORY_DROP_DXIL           = '1'
  VICTORY_DESC_DATA           = '1'
  VICTORY_BIND_CACHE          = '1'
  VICTORY_NULL_MAGENTA        = '1'

  # ---- Copy / upload / streaming -------------------------------------------
  VICTORY_RELAY               = '2'
  VICTORY_RELAY_OWN_QUEUE     = '1'
  VICTORY_WIDE_MERGE          = '1'
  VICTORY_WIDE_REFRESH        = '1'
  VICTORY_JOIN_COPIES         = '1'
  VICTORY_CARD_REHASH_RUNS    = '1'
  VICTORY_COPY_CHANGED        = '1'
  VICTORY_QUIET_BACK          = '1'
  VICTORY_FOLD_BUFFERS        = '1'
  VICTORY_WINDOW_TAILS        = '1'
  VICTORY_WINDOW_KB           = '1024'

  # ---- Listas / indirect ---------------------------------------------------
  VICTORY_THIN                = '1'
  VICTORY_PASSES_AHEAD        = '1'
  VICTORY_CLAMP_AHEAD         = '1'
  VICTORY_LIST_POOL_MOST      = '1024'
  VICTORY_LIST_POOL_MB        = '1024'
  VICTORY_LATE_WAIT_LISTS     = '1024'

  # ---- Streaming memory fix (Pool crash): espera maior por slots -----------
  VICTORY_DYNAMIC_WAIT_MS     = '60'

  # ---- Texturas / streaming ------------------------------------------------
  VICTORY_TEXTURE_NO_WAIT     = '1'
  VICTORY_TEXTURE_CATCH_UP_MS = '2'
  VICTORY_BAKE_TEXTURES_MS    = '10000'
  VICTORY_EARLY_TEXTURES_MS   = '40'
  VICTORY_REUPLOAD_WHEN_FULL  = '1'

  # ---- Misc Neptune --------------------------------------------------------
  VICTORY_ASK_ONCE            = '1'
  VICTORY_ASK_QUIET           = '8'
  VICTORY_D3D12_BACKGROUND    = '0'
  VICTORY_TEXTURE_HASH_EVERY  = '32'

  # ---- Ocean / agua --------------------------------------------------------
  VICTORY_OCEAN_READBACK      = '1'

  # ---- Monitor de page writes 4 KiB ----------------------------------------
  VICTORY_WATCH_4K            = '1'

  # ---- Auto-pulo de telas iniciais -----------------------------------------
  VICTORY_AUTO_A              = '300'

  # ---- RAGE C++ Shim (winmm.dll: FpsGovernor + ShaderStreamScheduler) ------
  SHIM_FPS_TARGET             = '10'
  SHIM_SHADER_BUDGET_MS       = '60'
  SHIM_HOT_THREADS            = '4'
  SHIM_WARM_THREADS           = '6'
  SHIM_DRAW_TIMEOUT_MS        = '8000'
  SHIM_VRAM_THROTTLE_PCT      = '82'
}

if ($NoThin) { $set.Remove('VICTORY_THIN'); $set.Remove('VICTORY_PASSES_AHEAD') }
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
    if ($in -and -not $done -and $lines[$i] -match ('^' + [regex]::Escape($Key) + '\s*=')) {
      $lines[$i] = "$Key=$Value"; $done = $true
    }
  }
  if (-not $done) { Write-Warning "OptiScaler.ini: no $Key in [$Section]" }
  $lines -join "`r`n"
}

function Initialize-Opti([string]$Exe, [string]$OptiDir, [string]$UpscalerName, [bool]$UseFrameGen) {
  $root = Split-Path $Exe -Parent
  $to   = Join-Path $root 'opti'
  New-Item -ItemType Directory -Force $to | Out-Null
  $stamp = Join-Path $to 'optiscaler.from'
  $from  = (Get-Item (Join-Path $OptiDir 'OptiScaler.dll')).LastWriteTimeUtc.Ticks.ToString() + ' ' + $OptiDir
  if (-not (Test-Path $stamp) -or (Get-Content $stamp -Raw).Trim() -ne $from) {
    Get-ChildItem $OptiDir -File | Where-Object { $_.Extension -in '.dll', '.ini' } | Copy-Item -Destination $to -Force
    Get-ChildItem $OptiDir -Directory | Where-Object { $_.Name -ne 'Licenses' } | Copy-Item -Destination $to -Recurse -Force
    Copy-Item (Join-Path $OptiDir 'OptiScaler.dll') (Join-Path $to 'dxgi.dll') -Force
    Remove-Item (Join-Path $to 'OptiScaler.dll') -Force -ErrorAction SilentlyContinue
    Set-Content $stamp $from
  }
  $ini = Get-Content (Join-Path $OptiDir 'OptiScaler.ini') -Raw
  $ini = Set-IniValue $ini 'Upscalers' 'Dx12Upscaler' $UpscalerName
  $ini = Set-IniValue $ini 'FrameGen'  'Enabled'      ($(if ($UseFrameGen) { 'true' } else { 'false' }))
  if ($UseFrameGen) {
    $ini = Set-IniValue $ini 'FrameGen' 'FGInput'       'upscaler'
    $ini = Set-IniValue $ini 'FrameGen' 'FGOutput'      'fsrfg'
    $ini = Set-IniValue $ini 'FSRFG'    'AllowAsync'    'true'
    $ini = Set-IniValue $ini 'OptiFG'   'DisableHUDFix' 'true'
  }
  Set-Content (Join-Path $to 'OptiScaler.ini') $ini -NoNewline
  $copy = Join-Path $to (Split-Path $Exe -Leaf)
  if (-not (Test-Path $copy) -or
      (Get-Item $copy).LastWriteTimeUtc -ne (Get-Item $Exe).LastWriteTimeUtc -or
      (Get-Item $copy).Length           -ne (Get-Item $Exe).Length) {
    Copy-Item $Exe $copy -Force
  }
  # Manter shims ao lado do exe em opti\ se existirem
  foreach ($extra in @('winmm.dll', 'nvngx_dlss.dll', 'amd_fidelityfx_upscaler_dx12.dll')) {
    $srcExtra = Join-Path $root $extra
    $dstExtra = Join-Path $to   $extra
    if ((Test-Path $srcExtra) -and
        (-not (Test-Path $dstExtra) -or
         (Get-Item $srcExtra).LastWriteTimeUtc -ne (Get-Item $dstExtra).LastWriteTimeUtc)) {
      Copy-Item $srcExtra $dstExtra -Force
    }
  }
  return $copy
}

$exe     = Join-Path $PSScriptRoot 'victory.exe'
$console = @()

if ($OptiScaler -and -not (Test-Path (Join-Path $OptiScaler 'OptiScaler.dll'))) {
  Write-Host "OptiScaler.dll nao encontrado em: $OptiScaler"
  exit 1
}
if ($FrameGen -and -not $OptiScaler) {
  Write-Host '-FrameGen requer -OptiScaler <pasta>'
  exit 1
}

if (-not $SeriesX) {
  $useOpti = $OptiScaler -and ($FrameGen -or $Upscaler -eq 'dlss')
  if ($useOpti) {
    $exe     = Initialize-Opti $exe (Resolve-Path $OptiScaler).Path $Upscaler ([bool]$FrameGen)
    $optiDir = Split-Path $exe -Parent
    $optiFsr = Join-Path $optiDir 'amd_fidelityfx_dx12.dll'
    if (Test-Path $optiFsr) {
      $FsrDll = $optiFsr
    } else {
      $FsrDll = Join-Path $optiDir 'amd_fidelityfx_upscaler_dx12.dll'
    }
    Write-Host "[Neptune] OptiScaler ativo: Upscaler=$Upscaler | FrameGen=$([bool]$FrameGen)" -ForegroundColor Green
  } else {
    if ($Upscaler -eq 'dlss' -and -not $OptiScaler) {
      Write-Host '[Neptune] AVISO: OptiScaler.dll nao encontrado - usando FSR padrao' -ForegroundColor Yellow
    }
    if (-not $FsrDll) {
      $candidates = @(Join-Path $PSScriptRoot 'amd_fidelityfx_upscaler_dx12.dll')
      if ($OptiScaler) { $candidates = @(Join-Path $OptiScaler 'amd_fidelityfx_upscaler_dx12.dll') + $candidates }
      $FsrDll = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    }
  }
  if ($FsrDll -and (Test-Path $FsrDll)) {
    $set['VICTORY_FSR_DLL'] = (Resolve-Path $FsrDll).Path
    $console = @('--console', 'series-s')
    $set['VICTORY_RENDER_S'] = '1'
    Write-Host '[Neptune] Modo: Xbox Series S (720p -> 1080p FSR)' -ForegroundColor Cyan
  } else {
    Write-Host '[Neptune] FSR DLL nao encontrada - iniciando Series X (4K)' -ForegroundColor Yellow
  }
} else {
  Write-Host '[Neptune] Modo forcado: Xbox Series X (4K)' -ForegroundColor Magenta
}

# Arquivo de luz ambiente (lido a cada 1s pelo runtime)
$amb = Join-Path $PSScriptRoot 'ambient.txt'
if (-not (Test-Path $amb)) {
  Set-Content -Path $amb -Value '0.36 0.05 0.4 1.0 0.6 0.1 0.45 0.5' -Encoding ascii
}
$set['VICTORY_AMBIENT_FILE'] = $amb

# Aplica todas as variaveis de ambiente
foreach ($k in $set.Keys) { Set-Item -Path "Env:$k" -Value $set[$k] }
$env:VICTORY_GAME_DIR = Split-Path -Parent (Resolve-Path -LiteralPath $Game)

if (Get-Process -Name victory -ErrorAction SilentlyContinue) {
  Write-Host 'O jogo ja esta em execucao.'; exit 1
}

Set-Location $PSScriptRoot

if ($Warm) {
  Write-Host '[Neptune] Pre-aquecendo shaders (pode levar ~7 minutos)...' -ForegroundColor Yellow
  & $exe warm
}

Write-Host '[Neptune] Iniciando GTA VI...' -ForegroundColor Green
& $exe run $Game --execute @console

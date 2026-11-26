# Neptune (VIctory Edit.) launcher - Adaptado para RTX 3060 12GB + i5-14600KF (32GB RAM)
#
# Uso rapido (usa o caminho padrao do jogo):
#   .\play.ps1
#   .\play.ps1 -Warm          (primeira vez: pre-traduz os shaders no cache em ~7 min)
#   .\play.ps1 -FrameGen      (se colocar a pasta OptiScaler ao lado do script)
#   .\play.ps1 -SeriesX       (forca 4K mesmo se houver DLL FSR)
#   .\play.ps1 -FrametimeDump (gera frametime.csv no cache)
#
# Controles (teclado e mouse):
#   Clique na tela ou F9 para travar/soltar o mouse (Alt+Tab tambem solta)
#   WASD move, mouse olha, Botao Esq/Dir = Gatilho Dir/Esq, Shift/Ctrl = clicks dos analogicos
#   Espaco/Backspace/X/Y = A/B/X/Y, Q/E = LB/RB, Esc = Menu, Tab = View
#   F10 oculta o HUD de FPS/memoria, F12 salva screenshot.

param(
  [string]$Game = 'F:\jogo\grand theft auto 6\Games\GTA 6\game_scarlett_master.exe',
  [switch]$Warm,                                  # traduz os shaders para o cache antes de abrir
  [switch]$NoAutoA,                               # nao pressiona A automaticamente nas telas iniciais
  [switch]$SeriesX,                               # forca modo Series X (4K)
  [string]$FsrDll = '',                           # caminho para amd_fidelityfx_upscaler_dx12.dll
  [string]$OptiScaler = '',                       # pasta de um release extraido do OptiScaler
  [switch]$FrameGen,                              # com OptiScaler: usa upscaler + frame generation
  [string]$Upscaler = 'xess',                     # com -FrameGen: xess, fsr31 ou dlss
  [switch]$NoThin,                                # traduz draw calls nas threads originais do jogo
  [switch]$FrametimeDump                          # grava <cache>\frametime.csv
)

if (-not (Test-Path -LiteralPath $Game)) { "Executavel do jogo nao encontrado: $Game"; exit 1 }

# Se houver uma pasta OptiScaler ao lado deste script e -OptiScaler nao foi passado, detecta automaticamente
if (-not $OptiScaler) {
  $localOpti = Join-Path $PSScriptRoot 'OptiScaler'
  if (Test-Path (Join-Path $localOpti 'OptiScaler.dll')) {
    $OptiScaler = $localOpti
  }
}

$set = [ordered]@{
  # Ajustado para RTX 3060 12 GB VRAM (padrao Neptune era 4096 / 1536 para 16 GB)
  VICTORY_CARD_WINDOWS_MB     = '2048'
  VICTORY_CARD_BUFFERS_MB     = '1024'
  VICTORY_CARD_BUFFERS_MIN_KB = '64'
  VICTORY_TEXTURE_STAGE_MB    = '512'

  # Threads e pipeline (otimizado para i5-14600KF 14c/20t)
  VICTORY_WIDE_THREADS        = '12'
  VICTORY_BAKE_TEXTURES_MS    = '10000'
  VICTORY_EARLY_TEXTURES_MS   = '40'
  VICTORY_WAVE_OWN_LDS_GROUPS = '64,128'
  VICTORY_WIDE_MERGE          = '1'
  VICTORY_RELAY_OWN_QUEUE     = '1'
  VICTORY_UNDER_WRITES_WORDS  = '1'
  VICTORY_UNDER_WRITES_RUNS   = '100000'
  VICTORY_UNDER_WRITES_MASKS  = '1'
  VICTORY_RELAY               = '2'
  VICTORY_WIDE_REFRESH        = '1'
  VICTORY_ASK_ONCE            = '1'
  VICTORY_TEXTURE_NO_WAIT     = '1'
  VICTORY_CARD_REHASH_RUNS    = '1'
  VICTORY_JOIN_COPIES         = '1'
  VICTORY_D3D12_BACKGROUND    = '0'
  VICTORY_LATE_WAIT_LISTS     = '1024'
  VICTORY_WINDOW_TAILS        = '1'
  VICTORY_ASK_QUIET           = '8'
  VICTORY_COPY_CHANGED        = '1'
  VICTORY_QUIET_BACK          = '1'
  VICTORY_LIST_POOL_MOST      = '2048'
  VICTORY_LIST_POOL_MB        = '2048'
  VICTORY_UNDER_WRITES_MASKS_LEAST = '1'
  VICTORY_INDIRECT_RAW        = '2'
  VICTORY_TEXTURE_HASH_EVERY  = '32'

  # Melhorias Neptune 2026-11-25
  VICTORY_LEVELS_ON_DEMAND    = '1'
  VICTORY_WINDOW_KB           = '1024'
  VICTORY_CARD_WINDOW_UPLOAD  = '1'
  VICTORY_DROP_DXIL           = '1'
  VICTORY_DESC_DATA           = '1'
  VICTORY_VRAM_IDLE_MS        = '15000,2500,1000'
  VICTORY_PIPELINE_WAIT_MS    = '2500'
  VICTORY_OCEAN_READBACK      = '1'
  VICTORY_WATCH_4K            = '1'
  VICTORY_THIN                = '1'
}
if (-not $NoAutoA) { $set['VICTORY_AUTO_A'] = '150' }
if ($FrametimeDump) { $set['VICTORY_FRAMETIME_DUMP'] = '1' }

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
  $ini = Set-IniValue $ini 'Upscalers' 'Dx12Upscaler' $Upscaler
  $ini = Set-IniValue $ini 'FrameGen' 'Enabled' 'true'
  $ini = Set-IniValue $ini 'FrameGen' 'FGInput' 'upscaler'
  $ini = Set-IniValue $ini 'FrameGen' 'FGOutput' 'fsrfg'
  $ini = Set-IniValue $ini 'FSRFG' 'AllowAsync' 'true'
  $ini = Set-IniValue $ini 'OptiFG' 'DisableHUDFix' 'true'
  Set-Content (Join-Path $to 'OptiScaler.ini') $ini -NoNewline
  $copy = Join-Path $to (Split-Path $Exe -Leaf)
  if (-not (Test-Path $copy) -or (Get-Item $copy).LastWriteTimeUtc -ne (Get-Item $Exe).LastWriteTimeUtc -or (Get-Item $copy).Length -ne (Get-Item $Exe).Length) {
    Copy-Item $Exe $copy -Force
  }
  $copy
}

$exe = Join-Path $PSScriptRoot '..\target\release\victory.exe'
if (-not (Test-Path $exe)) { $exe = Join-Path $PSScriptRoot '..\..\victory.exe' }
$console = @()
if ($OptiScaler -and -not (Test-Path (Join-Path $OptiScaler 'OptiScaler.dll'))) { "OptiScaler.dll nao encontrado em $OptiScaler"; exit 1 }
if ($FrameGen -and -not $OptiScaler) { '-FrameGen requer -OptiScaler <pasta>'; exit 1 }
if (-not $SeriesX) {
  if ($FrameGen) {
    $exe = Initialize-Opti $exe (Resolve-Path $OptiScaler).Path $Upscaler
    $FsrDll = Join-Path (Split-Path $exe -Parent) 'amd_fidelityfx_dx12.dll'
  } elseif (-not $FsrDll) {
    $near = @((Join-Path $PSScriptRoot 'amd_fidelityfx_upscaler_dx12.dll'))
    if ($OptiScaler) { $near = @((Join-Path $OptiScaler 'amd_fidelityfx_upscaler_dx12.dll')) + $near }
    $FsrDll = $near | Where-Object { Test-Path $_ } | Select-Object -First 1
  }
  if ($FsrDll -and (Test-Path $FsrDll)) {
    $set['VICTORY_FSR_DLL'] = (Resolve-Path $FsrDll).Path
    $console = @('--console', 'series-s')
  } else {
    Write-Host 'Aviso: amd_fidelityfx_upscaler_dx12.dll nao encontrada. Iniciando em Series X (4K).' -ForegroundColor Yellow
  }
}

$amb = Join-Path $PSScriptRoot 'ambient.txt'
if (-not (Test-Path $amb)) { Set-Content -Path $amb -Value '0.36 0.05 0.4 1.0 0.6 0.1 0.45 0.5' -Encoding ascii }
$set['VICTORY_AMBIENT_FILE'] = $amb
if ($NoThin) { $set.Remove('VICTORY_THIN') }
foreach ($k in $set.Keys) { Set-Item -Path ("Env:" + $k) -Value $set[$k] }
$env:VICTORY_GAME_DIR = Split-Path -Parent (Resolve-Path -LiteralPath $Game)
if (Get-Process victory -ErrorAction SilentlyContinue) { 'O jogo ja esta em execucao.'; exit 1 }
Set-Location $PSScriptRoot
if ($Warm) { & (Join-Path $PSScriptRoot 'victory.exe') warm }
& $exe run $Game --execute @console

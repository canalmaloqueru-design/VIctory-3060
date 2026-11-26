#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

namespace rage {

// Inicializa hooks em D3D12CreateDevice / CreateDXGIFactory1 / IDXGISwapChain::Present
// e ID3D12Device::CreateGraphicsPipelineState / CreateComputePipelineState
bool install_d3d12_hooks();
void remove_d3d12_hooks();

} // namespace rage

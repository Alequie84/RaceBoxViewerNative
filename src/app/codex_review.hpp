#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include <string>

namespace racebox::app::codex_review {

struct CaptureResult {
    bool ok{};
    std::string error;
};

// Captures the already-rendered DX11 back buffer. This is a one-way local
// handoff: no listener, command route, credentials, telemetry samples, or
// source paths are accepted by this contract.
CaptureResult capture(ID3D11Device* device, ID3D11DeviceContext* context,
                      IDXGISwapChain* swap_chain,
                      const std::string& bounded_context_json) noexcept;

}  // namespace racebox::app::codex_review

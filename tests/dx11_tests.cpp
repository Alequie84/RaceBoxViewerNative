#include <d3d11.h>

#include <iostream>

int main() {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL selected{};
    const D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    const auto result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, requested, 2,
        D3D11_SDK_VERSION, &device, &selected, &context);
    if (context) context->Release();
    if (device) device->Release();
    if (FAILED(result)) {
        std::cerr << "Microsoft WARP DirectX fallback failed: 0x" << std::hex << result << '\n';
        return 1;
    }
    std::cout << "Microsoft WARP DirectX fallback is available\n";
    return 0;
}

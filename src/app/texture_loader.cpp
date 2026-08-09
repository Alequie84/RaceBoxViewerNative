#include "native_app.hpp"
#include "qrcodegen.hpp"

#include <wincodec.h>

namespace racebox::app {

void Texture::reset() {
    if (view) view->Release();
    if (resource) resource->Release();
    view = nullptr;
    resource = nullptr;
    width = 0;
    height = 0;
}

bool load_texture(ID3D11Device* device, const std::filesystem::path& path, Texture& texture, std::string& error) {
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    ID3D11Texture2D* gpu_texture = nullptr;
    auto cleanup = [&] {
        if (gpu_texture) gpu_texture->Release();
        if (converter) converter->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (factory) factory->Release();
    };
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        cleanup();
        error = "Could not decode map image";
        return false;
    }
    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);
    if (!width || !height || width > 8192 || height > 8192) {
        cleanup();
        error = "Map image must be between 1 and 8192 pixels per side";
        return false;
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()))) {
        cleanup(); error = "Could not read map pixels"; return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width; description.Height = height; description.MipLevels = 1; description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM; description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE; description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels.data(), width * 4, 0};
    if (FAILED(device->CreateTexture2D(&description, &data, &gpu_texture))) {
        cleanup(); error = "Could not upload map image to DirectX"; return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC view_description{};
    view_description.Format = description.Format;
    view_description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view_description.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* view = nullptr;
    if (FAILED(device->CreateShaderResourceView(gpu_texture, &view_description, &view))) {
        cleanup(); error = "Could not create map texture view"; return false;
    }
    texture.reset();
    texture.resource = gpu_texture;
    texture.view = view;
    texture.width = static_cast<int>(width);
    texture.height = static_cast<int>(height);
    gpu_texture = nullptr;
    cleanup();
    return true;
}

bool create_bgra_texture(
    ID3D11Device* device, int width, int height, int stride,
    const std::vector<std::uint8_t>& pixels, Texture& texture,
    std::string& error) {
    if (!device || width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        stride < width * 4 || pixels.size() <
            static_cast<std::size_t>(stride) * static_cast<std::size_t>(height)) {
        error = "The setup-sheet preview bitmap is invalid";
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = static_cast<UINT>(width);
    description.Height = static_cast<UINT>(height);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{
        pixels.data(), static_cast<UINT>(stride), 0};
    ID3D11Texture2D* gpu_texture = nullptr;
    if (FAILED(device->CreateTexture2D(
            &description, &data, &gpu_texture))) {
        error = "Could not upload the setup-sheet preview to DirectX";
        return false;
    }
    ID3D11ShaderResourceView* view = nullptr;
    if (FAILED(device->CreateShaderResourceView(
            gpu_texture, nullptr, &view))) {
        gpu_texture->Release();
        error = "Could not display the setup-sheet preview";
        return false;
    }
    texture.reset();
    texture.resource = gpu_texture;
    texture.view = view;
    texture.width = width;
    texture.height = height;
    error.clear();
    return true;
}

bool create_qr_texture(
    ID3D11Device* device,
    const std::string& value,
    Texture& texture,
    std::string& error) {
    if (!device || value.empty()) {
        error = "Pairing link is unavailable";
        return false;
    }
    try {
        const auto qr = qrcodegen::QrCode::encodeText(
            value.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        constexpr int border = 4;
        constexpr int module_pixels = 6;
        const int modules = qr.getSize() + border * 2;
        const int side = modules * module_pixels;
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(side) * side * 4, 255);
        for (int y = 0; y < side; ++y) {
            for (int x = 0; x < side; ++x) {
                const bool dark = qr.getModule(
                    x / module_pixels - border,
                    y / module_pixels - border);
                const auto pixel =
                    (static_cast<std::size_t>(y) * side + x) * 4;
                const std::uint8_t value_byte = dark ? 0 : 255;
                pixels[pixel] = value_byte;
                pixels[pixel + 1] = value_byte;
                pixels[pixel + 2] = value_byte;
                pixels[pixel + 3] = 255;
            }
        }

        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(side);
        description.Height = static_cast<UINT>(side);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{
            pixels.data(), static_cast<UINT>(side * 4), 0};
        ID3D11Texture2D* gpu_texture = nullptr;
        if (FAILED(device->CreateTexture2D(&description, &data, &gpu_texture))) {
            error = "Could not upload the pairing QR code";
            return false;
        }
        ID3D11ShaderResourceView* view = nullptr;
        if (FAILED(device->CreateShaderResourceView(
                gpu_texture, nullptr, &view))) {
            gpu_texture->Release();
            error = "Could not display the pairing QR code";
            return false;
        }
        texture.reset();
        texture.resource = gpu_texture;
        texture.view = view;
        texture.width = side;
        texture.height = side;
        error.clear();
        return true;
    } catch (const std::exception&) {
        error = "Could not encode the pairing QR code";
        return false;
    }
}

}  // namespace racebox::app

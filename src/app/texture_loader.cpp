#include "native_app.hpp"

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

}  // namespace racebox::app

#include "codex_review.hpp"

#include "racebox/core.hpp"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace racebox::app::codex_review {
namespace {

using Microsoft::WRL::ComPtr;

std::string windows_error(const char* operation, HRESULT result) {
    return std::string(operation) + " failed (HRESULT " +
        std::to_string(static_cast<unsigned long>(result)) + ")";
}

bool replace_file(const std::filesystem::path& temporary,
                  const std::filesystem::path& destination,
                  std::string& error) {
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    error = "Could not atomically replace the Codex Review file";
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
}

bool write_png(ID3D11Device* device, ID3D11DeviceContext* context,
               IDXGISwapChain* swap_chain,
               const std::filesystem::path& destination,
               std::string& error) {
    ComPtr<ID3D11Texture2D> back_buffer;
    auto result = swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(result)) {
        error = windows_error("Reading the rendered window", result);
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    back_buffer->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    ComPtr<ID3D11Texture2D> staging;
    result = device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) {
        error = windows_error("Creating the review staging texture", result);
        return false;
    }
    context->CopyResource(staging.Get(), back_buffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) {
        error = windows_error("Mapping the review staging texture", result);
        return false;
    }

    const auto output_stride = description.Width * 4U;
    std::vector<BYTE> bgra(
        static_cast<std::size_t>(output_stride) * description.Height);
    for (UINT y = 0; y < description.Height; ++y) {
        const auto* source = static_cast<const BYTE*>(mapped.pData) +
            static_cast<std::size_t>(mapped.RowPitch) * y;
        auto* destination_row = bgra.data() +
            static_cast<std::size_t>(output_stride) * y;
        for (UINT x = 0; x < description.Width; ++x) {
            destination_row[x * 4U + 0U] = source[x * 4U + 2U];
            destination_row[x * 4U + 1U] = source[x * 4U + 1U];
            destination_row[x * 4U + 2U] = source[x * 4U + 0U];
            destination_row[x * 4U + 3U] = source[x * 4U + 3U];
        }
    }
    context->Unmap(staging.Get(), 0);

    ComPtr<IWICImagingFactory> factory;
    result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        error = windows_error("Starting Windows image encoding", result);
        return false;
    }
    auto temporary = destination;
    temporary += L".writing";
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    result = factory->CreateStream(&stream);
    if (SUCCEEDED(result)) {
        result = stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE);
    }
    if (SUCCEEDED(result)) {
        result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    }
    if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, &properties);
    if (SUCCEEDED(result)) result = frame->Initialize(properties.Get());
    if (SUCCEEDED(result)) result = frame->SetSize(description.Width, description.Height);
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(result)) result = frame->SetPixelFormat(&pixel_format);
    if (SUCCEEDED(result) && pixel_format != GUID_WICPixelFormat32bppBGRA) {
        result = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    }
    if (SUCCEEDED(result)) {
        const auto byte_count = static_cast<UINT>(bgra.size());
        result = frame->WritePixels(description.Height, output_stride,
                                    byte_count, bgra.data());
    }
    if (SUCCEEDED(result)) result = frame->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    stream.Reset();
    frame.Reset();
    encoder.Reset();
    if (FAILED(result)) {
        std::filesystem::remove(temporary, ignored);
        error = windows_error("Encoding the Codex Review PNG", result);
        return false;
    }
    return replace_file(temporary, destination, error);
}

}  // namespace

CaptureResult capture(ID3D11Device* device, ID3D11DeviceContext* context,
                      IDXGISwapChain* swap_chain,
                      const std::string& bounded_context_json) noexcept {
    CaptureResult result;
    try {
        if (!device || !context || !swap_chain) {
            result.error = "The rendered window is unavailable";
            return result;
        }
        if (bounded_context_json.empty() || bounded_context_json.size() > 256 * 1024) {
            result.error = "The Codex Review context exceeded its safe bound";
            return result;
        }
        const auto directory = settings_directory() / L"codex-review";
        std::filesystem::create_directories(directory);
        if (!write_png(device, context, swap_chain,
                       directory / L"active-review.png", result.error)) {
            return result;
        }
        const auto json_path = directory / L"active-review.json";
        auto temporary = json_path;
        temporary += L".writing";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                result.error = "Could not create the Codex Review context file";
                return result;
            }
            output << bounded_context_json << '\n';
            output.flush();
            if (!output) {
                result.error = "Could not finish the Codex Review context file";
                return result;
            }
        }
        if (!replace_file(temporary, json_path, result.error)) return result;
        result.ok = true;
        return result;
    } catch (const std::exception& exception) {
        result.error = std::string("Codex Review capture failed: ") + exception.what();
        return result;
    } catch (...) {
        result.error = "Codex Review capture failed";
        return result;
    }
}

}  // namespace racebox::app::codex_review

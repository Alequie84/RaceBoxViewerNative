#include "racebox/pdf_setup_editor.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <fpdf_annot.h>
#include <fpdf_edit.h>
#include <fpdf_formfill.h>
#include <fpdf_save.h>
#include <fpdfview.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <type_traits>

namespace racebox::pdf_setup {
namespace {

constexpr std::uintmax_t kMaximumPdfBytes = 64ULL * 1024ULL * 1024ULL;

struct PdfiumLifetime {
    PdfiumLifetime() { FPDF_InitLibrary(); }
    ~PdfiumLifetime() { FPDF_DestroyLibrary(); }
};

void ensure_pdfium() {
    static PdfiumLifetime lifetime;
    (void)lifetime;
}

std::string pdf_error(std::string_view operation) {
    const char* reason = "unknown PDF error";
    switch (FPDF_GetLastError()) {
        case FPDF_ERR_SUCCESS: reason = "no PDFium error was reported"; break;
        case FPDF_ERR_UNKNOWN: reason = "unknown PDF error"; break;
        case FPDF_ERR_FILE: reason = "the file could not be read"; break;
        case FPDF_ERR_FORMAT: reason = "the PDF structure is invalid"; break;
        case FPDF_ERR_PASSWORD: reason = "the PDF requires a password"; break;
        case FPDF_ERR_SECURITY: reason = "the PDF security settings rejected the operation"; break;
        case FPDF_ERR_PAGE: reason = "a page could not be loaded"; break;
        default: break;
    }
    return std::string(operation) + ": " + reason + '.';
}

std::vector<std::uint8_t> read_pdf(const std::filesystem::path& path,
                                   std::string& error) {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(path, filesystem_error)) {
        error = "The setup PDF is missing or unreadable.";
        return {};
    }
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || size < 8 || size > kMaximumPdfBytes) {
        error = "The setup PDF must be between 8 bytes and 64 MB.";
        return {};
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        error = "The setup PDF could not be read completely.";
        return {};
    }
    return bytes;
}

struct DocumentCloser {
    void operator()(FPDF_DOCUMENT document) const {
        if (document) FPDF_CloseDocument(document);
    }
};
struct PageCloser {
    void operator()(FPDF_PAGE page) const {
        if (page) FPDF_ClosePage(page);
    }
};
struct BitmapCloser {
    void operator()(FPDF_BITMAP bitmap) const {
        if (bitmap) FPDFBitmap_Destroy(bitmap);
    }
};

using Document = std::unique_ptr<std::remove_pointer_t<FPDF_DOCUMENT>, DocumentCloser>;
using Page = std::unique_ptr<std::remove_pointer_t<FPDF_PAGE>, PageCloser>;
using Bitmap = std::unique_ptr<std::remove_pointer_t<FPDF_BITMAP>, BitmapCloser>;

Document load_document(const std::vector<std::uint8_t>& bytes,
                       std::string& error) {
    ensure_pdfium();
    auto* document = FPDF_LoadMemDocument64(
        bytes.data(), static_cast<size_t>(bytes.size()), nullptr);
    if (!document) error = pdf_error("PDFium could not open the setup sheet");
    return Document(document);
}

std::u16string utf16(std::string_view input) {
    if (input.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) return {};
    std::u16string output(static_cast<std::size_t>(count), u'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()),
        reinterpret_cast<wchar_t*>(output.data()), count);
    return output;
}

std::string utf8(std::span<const char16_t> input) {
    if (input.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, reinterpret_cast<const wchar_t*>(input.data()),
        static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string output(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, reinterpret_cast<const wchar_t*>(input.data()),
        static_cast<int>(input.size()), output.data(), count, nullptr, nullptr);
    return output;
}

template <typename Reader>
std::string read_wide(Reader&& reader) {
    const auto bytes = reader(nullptr, 0UL);
    if (bytes <= 2 || bytes > 64UL * 1024UL) return {};
    std::vector<char16_t> value((bytes + 1) / 2, u'\0');
    if (reader(reinterpret_cast<FPDF_WCHAR*>(value.data()), bytes) != bytes) {
        return {};
    }
    if (!value.empty() && value.back() == u'\0') value.pop_back();
    return utf8(value);
}

struct FormEnvironment {
    FPDF_FORMFILLINFO info{};
    FPDF_FORMHANDLE handle{};
    explicit FormEnvironment(FPDF_DOCUMENT document) {
        info.version = 1;
        handle = FPDFDOC_InitFormFillEnvironment(document, &info);
    }
    ~FormEnvironment() {
        if (handle) FPDFDOC_ExitFormFillEnvironment(handle);
    }
};

std::string form_type_name(int type) {
    switch (type) {
        case FPDF_FORMFIELD_TEXTFIELD: return "Text";
        case FPDF_FORMFIELD_CHECKBOX: return "Check box";
        case FPDF_FORMFIELD_RADIOBUTTON: return "Radio button";
        case FPDF_FORMFIELD_COMBOBOX: return "Choice";
        case FPDF_FORMFIELD_LISTBOX: return "List";
        case FPDF_FORMFIELD_SIGNATURE: return "Signature";
        default: return "Form";
    }
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool requested_checkbox_state(std::string_view value) {
    const auto normalized = lower(std::string(value));
    return normalized == "1" || normalized == "true" || normalized == "yes" ||
           normalized == "on" || normalized == "checked" || normalized == "x";
}

bool current_checkbox_state(std::string_view value) {
    const auto normalized = lower(std::string(value));
    return !(normalized.empty() || normalized == "off" || normalized == "false" ||
             normalized == "0" || normalized == "no");
}

struct FileWriter {
    FPDF_FILEWRITE base{};
    std::FILE* file{};
    static int write(FPDF_FILEWRITE* raw, const void* data, unsigned long size) {
        auto* writer = reinterpret_cast<FileWriter*>(raw);
        return writer->file &&
            std::fwrite(data, 1, size, writer->file) == size;
    }
};

bool save_document(FPDF_DOCUMENT document,
                   const std::filesystem::path& output,
                   std::string& error) {
    auto temporary = output;
    temporary += L".writing";
    std::FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || !file) {
        error = "Could not create the edited setup PDF.";
        return false;
    }
    FileWriter writer;
    writer.base.version = 1;
    writer.base.WriteBlock = &FileWriter::write;
    writer.file = file;
    const bool saved = FPDF_SaveAsCopy(
        document, &writer.base,
        FPDF_NO_INCREMENTAL | FPDF_SUBSET_NEW_FONTS) != 0;
    const bool closed = std::fclose(file) == 0;
    if (!saved || !closed) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = pdf_error("PDFium could not save the edited setup sheet");
        return false;
    }
    std::error_code filesystem_error;
    if (std::filesystem::exists(output, filesystem_error)) {
        std::filesystem::remove(temporary, filesystem_error);
        error = "The edited PDF destination already exists; no file was overwritten.";
        return false;
    }
    std::filesystem::rename(temporary, output, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary, filesystem_error);
        error = "Could not finalize the edited setup PDF: " + filesystem_error.message();
        return false;
    }
    return true;
}

bool add_flat_value(FPDF_DOCUMENT document, FPDF_PAGE page,
                    const race_day::SetupFieldValue& field) {
    const double page_width = FPDF_GetPageWidthF(page);
    const double page_height = FPDF_GetPageHeightF(page);
    const double left = std::clamp(field.left, 0.0, 1.0) * page_width;
    const double right = std::clamp(field.right, 0.0, 1.0) * page_width;
    const double top = (1.0 - std::clamp(field.top, 0.0, 1.0)) * page_height;
    const double bottom = (1.0 - std::clamp(field.bottom, 0.0, 1.0)) * page_height;
    if (right <= left || top <= bottom) return false;

    auto* cover = FPDFPageObj_CreateNewRect(
        static_cast<float>(left), static_cast<float>(bottom),
        static_cast<float>(right - left), static_cast<float>(top - bottom));
    if (!cover || !FPDFPageObj_SetFillColor(cover, 255, 255, 255, 255) ||
        !FPDFPath_SetDrawMode(cover, FPDF_FILLMODE_WINDING, false)) {
        if (cover) FPDFPageObj_Destroy(cover);
        return false;
    }
    FPDFPage_InsertObject(page, cover);

    const auto text = utf16(field.value);
    if (text.empty()) return FPDFPage_GenerateContent(page) != 0;
    const float font_size = static_cast<float>(
        std::clamp((top - bottom) * 0.68, 5.0, 24.0));
    auto* object = FPDFPageObj_NewTextObj(document, "Helvetica", font_size);
    if (!object || !FPDFText_SetText(
            object, reinterpret_cast<FPDF_WIDESTRING>(text.c_str())) ||
        !FPDFPageObj_SetFillColor(object, 0, 0, 0, 255)) {
        if (object) FPDFPageObj_Destroy(object);
        return false;
    }
    FPDFPageObj_Transform(
        object, 1.0, 0.0, 0.0, 1.0,
        left + 1.5, bottom + std::max(1.0, (top - bottom - font_size) * 0.45));
    FPDFPage_InsertObject(page, object);
    return FPDFPage_GenerateContent(page) != 0;
}

}  // namespace

Inspection inspect(const std::filesystem::path& source) {
    Inspection result;
    auto bytes = read_pdf(source, result.error);
    if (bytes.empty()) return result;
    auto document = load_document(bytes, result.error);
    if (!document) return result;
    FormEnvironment form(document.get());
    result.has_acroform = FPDF_GetFormType(document.get()) == FORMTYPE_ACRO_FORM;
    const int page_count = FPDF_GetPageCount(document.get());
    if (page_count <= 0 || page_count > 100) {
        result.error = "The setup PDF has no usable pages or exceeds the 100-page safety limit.";
        return result;
    }
    result.pages.reserve(static_cast<std::size_t>(page_count));
    for (int page_index = 0; page_index < page_count; ++page_index) {
        Page page(FPDF_LoadPage(document.get(), page_index));
        if (!page) {
            result.error = pdf_error("PDFium could not load a setup-sheet page");
            return result;
        }
        if (form.handle) FORM_OnAfterLoadPage(page.get(), form.handle);
        const double width = FPDF_GetPageWidthF(page.get());
        const double height = FPDF_GetPageHeightF(page.get());
        result.pages.push_back({width, height});
        const int annotation_count = std::max(0, FPDFPage_GetAnnotCount(page.get()));
        for (int annotation_index = 0; annotation_index < annotation_count; ++annotation_index) {
            auto* annotation = FPDFPage_GetAnnot(page.get(), annotation_index);
            if (!annotation) continue;
            if (FPDFAnnot_GetSubtype(annotation) == FPDF_ANNOT_WIDGET && form.handle) {
                const auto name = read_wide([&](FPDF_WCHAR* output, unsigned long size) {
                    return FPDFAnnot_GetFormFieldName(form.handle, annotation, output, size);
                });
                const auto alternate = read_wide([&](FPDF_WCHAR* output, unsigned long size) {
                    return FPDFAnnot_GetFormFieldAlternateName(form.handle, annotation, output, size);
                });
                const auto value = read_wide([&](FPDF_WCHAR* output, unsigned long size) {
                    return FPDFAnnot_GetFormFieldValue(form.handle, annotation, output, size);
                });
                FS_RECTF rectangle{};
                if (!name.empty() && width > 0.0 && height > 0.0 &&
                    FPDFAnnot_GetRect(annotation, &rectangle)) {
                    const auto type = FPDFAnnot_GetFormFieldType(form.handle, annotation);
                    race_day::SetupFieldValue field;
                    field.key = "acro:" + name;
                    field.label = alternate.empty() ? name : alternate;
                    field.section = form_type_name(type);
                    field.value = value;
                    field.page = page_index;
                    field.left = std::clamp(static_cast<double>(rectangle.left) / width, 0.0, 1.0);
                    field.right = std::clamp(static_cast<double>(rectangle.right) / width, 0.0, 1.0);
                    field.top = std::clamp(1.0 - static_cast<double>(rectangle.top) / height, 0.0, 1.0);
                    field.bottom = std::clamp(1.0 - static_cast<double>(rectangle.bottom) / height, 0.0, 1.0);
                    field.confidence = 100;
                    field.confirmed = true;
                    result.fields.push_back(std::move(field));
                }
            }
            FPDFPage_CloseAnnot(annotation);
        }
        if (form.handle) FORM_OnBeforeClosePage(page.get(), form.handle);
    }
    result.ok = true;
    return result;
}

RenderedPage render_page(const std::filesystem::path& source,
                         int page_index, int maximum_dimension) {
    RenderedPage result;
    result.page = page_index;
    auto bytes = read_pdf(source, result.error);
    if (bytes.empty()) return result;
    auto document = load_document(bytes, result.error);
    if (!document) return result;
    if (page_index < 0 || page_index >= FPDF_GetPageCount(document.get())) {
        result.error = "The requested setup-sheet page does not exist.";
        return result;
    }
    Page page(FPDF_LoadPage(document.get(), page_index));
    if (!page) {
        result.error = pdf_error("PDFium could not load the setup-sheet page");
        return result;
    }
    const double page_width = FPDF_GetPageWidthF(page.get());
    const double page_height = FPDF_GetPageHeightF(page.get());
    if (page_width <= 0.0 || page_height <= 0.0) {
        result.error = "The setup-sheet page has invalid dimensions.";
        return result;
    }
    maximum_dimension = std::clamp(maximum_dimension, 256, 3'200);
    const double scale = static_cast<double>(maximum_dimension) /
                         std::max(page_width, page_height);
    result.width = std::max(1, static_cast<int>(std::lround(page_width * scale)));
    result.height = std::max(1, static_cast<int>(std::lround(page_height * scale)));
    Bitmap bitmap(FPDFBitmap_Create(result.width, result.height, 1));
    if (!bitmap) {
        result.error = "PDFium could not allocate the setup-sheet preview.";
        return result;
    }
    FPDFBitmap_FillRect(bitmap.get(), 0, 0, result.width, result.height, 0xFFFFFFFF);
    FormEnvironment form(document.get());
    if (form.handle) FORM_OnAfterLoadPage(page.get(), form.handle);
    constexpr int flags = FPDF_ANNOT | FPDF_LCD_TEXT;
    FPDF_RenderPageBitmap(bitmap.get(), page.get(), 0, 0,
                          result.width, result.height, 0, flags);
    if (form.handle) {
        FPDF_FFLDraw(form.handle, bitmap.get(), page.get(), 0, 0,
                     result.width, result.height, 0, flags);
        FORM_OnBeforeClosePage(page.get(), form.handle);
    }
    result.stride = FPDFBitmap_GetStride(bitmap.get());
    const auto* buffer = static_cast<const std::uint8_t*>(
        FPDFBitmap_GetBuffer(bitmap.get()));
    if (!buffer || result.stride < result.width * 4) {
        result.error = "PDFium returned an invalid setup-sheet bitmap.";
        return result;
    }
    result.bgra.assign(buffer, buffer +
        static_cast<std::size_t>(result.stride) * static_cast<std::size_t>(result.height));
    result.ok = true;
    return result;
}

std::vector<std::uint8_t> encode_png(
    const RenderedPage& page, std::string& error) {
    error.clear();
    if (!page.ok || page.width <= 0 || page.height <= 0 ||
        page.stride < page.width * 4 ||
        page.bgra.size() < static_cast<std::size_t>(page.stride) *
            static_cast<std::size_t>(page.height)) {
        error = "The rendered setup-sheet page is invalid.";
        return {};
    }

    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto uninitialize = initialized == S_OK || initialized == S_FALSE;
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        error = "Windows could not start the setup-sheet image encoder.";
        return {};
    }
    struct ComScope {
        bool uninitialize{};
        ~ComScope() { if (uninitialize) CoUninitialize(); }
    } com_scope{uninitialize};
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    auto status = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(status)) {
        error = "Windows could not create the setup-sheet image encoder.";
        return {};
    }
    ComPtr<IStream> stream;
    status = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(status)) {
        error = "Windows could not allocate the setup-sheet image.";
        return {};
    }
    ComPtr<IWICBitmapEncoder> encoder;
    status = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(status)) {
        status = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    }
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    if (SUCCEEDED(status)) status = encoder->CreateNewFrame(&frame, &options);
    if (SUCCEEDED(status)) status = frame->Initialize(options.Get());
    if (SUCCEEDED(status)) {
        status = frame->SetSize(
            static_cast<UINT>(page.width), static_cast<UINT>(page.height));
    }
    auto pixel_format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(status)) status = frame->SetPixelFormat(&pixel_format);
    if (SUCCEEDED(status) &&
        !IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppBGRA)) {
        status = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    }
    if (SUCCEEDED(status)) {
        status = frame->WritePixels(
            static_cast<UINT>(page.height), static_cast<UINT>(page.stride),
            static_cast<UINT>(page.bgra.size()),
            const_cast<BYTE*>(page.bgra.data()));
    }
    if (SUCCEEDED(status)) status = frame->Commit();
    if (SUCCEEDED(status)) status = encoder->Commit();
    STATSTG stream_status{};
    if (SUCCEEDED(status)) status = stream->Stat(&stream_status, STATFLAG_NONAME);
    if (FAILED(status) || stream_status.cbSize.QuadPart <= 0 ||
        stream_status.cbSize.QuadPart > 3 * 1024 * 1024) {
        error = FAILED(status)
            ? "Windows could not encode the setup-sheet page as PNG."
            : "The rendered setup-sheet page is too large to send.";
        return {};
    }
    LARGE_INTEGER beginning{};
    status = stream->Seek(beginning, STREAM_SEEK_SET, nullptr);
    std::vector<std::uint8_t> result(
        static_cast<std::size_t>(stream_status.cbSize.QuadPart));
    ULONG read = 0;
    if (SUCCEEDED(status)) {
        status = stream->Read(result.data(), static_cast<ULONG>(result.size()), &read);
    }
    if (FAILED(status) || read != result.size()) {
        error = "Windows could not read the encoded setup-sheet image.";
        return {};
    }
    return result;
}

bool save_values(const std::filesystem::path& input,
                 const std::filesystem::path& output,
                 const std::vector<race_day::SetupFieldValue>& fields,
                 std::string& error) {
    error.clear();
    std::error_code filesystem_error;
    if (std::filesystem::equivalent(input, output, filesystem_error)) {
        error = "The original setup PDF is immutable; choose a new output file.";
        return false;
    }
    auto bytes = read_pdf(input, error);
    if (bytes.empty()) return false;
    auto document = load_document(bytes, error);
    if (!document) return false;
    FormEnvironment form(document.get());
    const int page_count = FPDF_GetPageCount(document.get());
    bool changed = false;
    for (int page_index = 0; page_index < page_count; ++page_index) {
        Page page(FPDF_LoadPage(document.get(), page_index));
        if (!page) {
            error = pdf_error("PDFium could not edit a setup-sheet page");
            return false;
        }
        if (form.handle) FORM_OnAfterLoadPage(page.get(), form.handle);
        const int annotation_count = std::max(0, FPDFPage_GetAnnotCount(page.get()));
        for (int annotation_index = 0; annotation_index < annotation_count; ++annotation_index) {
            auto* annotation = FPDFPage_GetAnnot(page.get(), annotation_index);
            if (!annotation) continue;
            if (FPDFAnnot_GetSubtype(annotation) == FPDF_ANNOT_WIDGET && form.handle) {
                const auto name = read_wide([&](FPDF_WCHAR* output_buffer, unsigned long size) {
                    return FPDFAnnot_GetFormFieldName(
                        form.handle, annotation, output_buffer, size);
                });
                const auto found = std::find_if(fields.begin(), fields.end(),
                    [&](const auto& field) {
                        return field.confirmed && field.page == page_index &&
                               field.key == "acro:" + name;
                    });
                if (found != fields.end()) {
                    const int type = FPDFAnnot_GetFormFieldType(form.handle, annotation);
                    if (type == FPDF_FORMFIELD_TEXTFIELD || type == FPDF_FORMFIELD_COMBOBOX) {
                        const auto value = utf16(found->value);
                        if (FORM_SetFocusedAnnot(form.handle, annotation) &&
                            FORM_SelectAllText(form.handle, page.get())) {
                            FORM_ReplaceSelection(
                                form.handle, page.get(),
                                reinterpret_cast<FPDF_WIDESTRING>(value.c_str()));
                            changed = FORM_ForceToKillFocus(form.handle) || changed;
                        }
                    } else if (type == FPDF_FORMFIELD_CHECKBOX ||
                               type == FPDF_FORMFIELD_RADIOBUTTON) {
                        const auto current = read_wide([&](FPDF_WCHAR* output_buffer, unsigned long size) {
                            return FPDFAnnot_GetFormFieldValue(
                                form.handle, annotation, output_buffer, size);
                        });
                        if (requested_checkbox_state(found->value) != current_checkbox_state(current)) {
                            FS_RECTF rectangle{};
                            if (FPDFAnnot_GetRect(annotation, &rectangle)) {
                                const double x = (rectangle.left + rectangle.right) * 0.5;
                                const double y = (rectangle.bottom + rectangle.top) * 0.5;
                                changed = FORM_OnLButtonDown(form.handle, page.get(), 0, x, y) &&
                                    FORM_OnLButtonUp(form.handle, page.get(), 0, x, y);
                            }
                        }
                    }
                }
            }
            FPDFPage_CloseAnnot(annotation);
        }
        for (const auto& field : fields) {
            if (field.confirmed && field.page == page_index &&
                field.key.starts_with("flat:")) {
                changed = add_flat_value(document.get(), page.get(), field) || changed;
            }
        }
        if (form.handle) FORM_OnBeforeClosePage(page.get(), form.handle);
    }
    if (!changed) {
        error = "No confirmed setup values matched this PDF.";
        return false;
    }
    return save_document(document.get(), output, error);
}

}  // namespace racebox::pdf_setup

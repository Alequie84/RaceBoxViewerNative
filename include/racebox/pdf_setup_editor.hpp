#pragma once

#include "racebox/race_day.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace racebox::pdf_setup {

struct PageInfo {
    double width_points{};
    double height_points{};
};

struct Inspection {
    bool ok{};
    bool has_acroform{};
    std::vector<PageInfo> pages;
    std::vector<race_day::SetupFieldValue> fields;
    std::string error;
};

struct RenderedPage {
    bool ok{};
    int page{};
    int width{};
    int height{};
    int stride{};
    // PDFium's native 32-bit bitmap order. On little-endian Windows this is
    // BGRA and can be uploaded directly to a DXGI B8G8R8A8 texture.
    std::vector<std::uint8_t> bgra;
    std::string error;
};

[[nodiscard]] Inspection inspect(const std::filesystem::path& source);
[[nodiscard]] RenderedPage render_page(const std::filesystem::path& source,
                                       int page,
                                       int maximum_dimension = 1'600);

// Encodes an already-rendered page without writing a temporary image or
// exposing a local path. Intended for an explicit bounded Crew Chief action.
[[nodiscard]] std::vector<std::uint8_t> encode_png(
    const RenderedPage& page, std::string& error);

// Applies confirmed values and writes a new PDF. Keys beginning with "acro:"
// address existing AcroForm widgets. Confirmed "flat:" fields are written as
// a bounded overlay inside their mapped rectangle. The input is never changed.
bool save_values(const std::filesystem::path& input,
                 const std::filesystem::path& output,
                 const std::vector<race_day::SetupFieldValue>& fields,
                 std::string& error);

}  // namespace racebox::pdf_setup

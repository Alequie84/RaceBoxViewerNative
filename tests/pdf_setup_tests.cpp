#include "racebox/pdf_setup_editor.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::high_resolution_clock::now()
                                .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("racebox-pdf-setup-" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void write_fillable_fixture(const std::filesystem::path& path) {
    const std::vector<std::string> objects{
        "<< /Type /Catalog /Pages 2 0 R /AcroForm 6 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /Helv 7 0 R >> >> /Contents 4 0 R /Annots [5 0 R] >>",
        "<< /Length 0 >>\nstream\n\nendstream",
        "<< /Type /Annot /Subtype /Widget /FT /Tx /T (RearSpring) /TU (Rear spring) /V (2.6) /Rect [100 500 220 530] /P 3 0 R /DA (/Helv 12 Tf 0 g) >>",
        "<< /Fields [5 0 R] /NeedAppearances true /DR << /Font << /Helv 7 0 R >> >> /DA (/Helv 12 Tf 0 g) >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    };
    std::string pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets{0};
    for (std::size_t index = 0; index < objects.size(); ++index) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(index + 1) + " 0 obj\n" + objects[index] + "\nendobj\n";
    }
    const auto xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n";
    pdf += "0000000000 65535 f \n";
    for (std::size_t index = 1; index < offsets.size(); ++index) {
        std::ostringstream row;
        row << std::setw(10) << std::setfill('0') << offsets[index]
            << " 00000 n \n";
        pdf += row.str();
    }
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
}

}  // namespace

int main() {
    TemporaryDirectory temporary;
    const auto input = temporary.path() / "fillable.pdf";
    const auto edited = temporary.path() / "edited.pdf";
    const auto flat_edited = temporary.path() / "flat-edited.pdf";
    write_fillable_fixture(input);

    const auto inspection = racebox::pdf_setup::inspect(input);
    require(inspection.ok && inspection.has_acroform &&
                inspection.pages.size() == 1 && inspection.fields.size() == 1,
            inspection.error.c_str());
    require(inspection.fields.front().key == "acro:RearSpring" &&
                inspection.fields.front().value == "2.6" &&
                inspection.fields.front().label == "Rear spring",
            "AcroForm name, value, or alternate label was not inspected");

    const auto rendered = racebox::pdf_setup::render_page(input, 0, 800);
    require(rendered.ok && rendered.width > 500 && rendered.height == 800 &&
                rendered.stride >= rendered.width * 4 && !rendered.bgra.empty(),
            rendered.error.c_str());
    std::string png_error;
    const auto png = racebox::pdf_setup::encode_png(rendered, png_error);
    require(png.size() > 8 && png[0] == 0x89 && png[1] == 0x50 &&
                png[2] == 0x4e && png[3] == 0x47,
            png_error.empty() ? "Rendered setup-sheet PNG was invalid" : png_error.c_str());

    auto fields = inspection.fields;
    fields.front().value = "2.8";
    std::string error;
    require(racebox::pdf_setup::save_values(input, edited, fields, error),
            error.c_str());
    const auto reopened = racebox::pdf_setup::inspect(edited);
    require(reopened.ok && reopened.fields.size() == 1 &&
                reopened.fields.front().value == "2.8",
            "Edited AcroForm value did not survive save and reopen");
    require(std::filesystem::is_regular_file(input),
            "The untouched source PDF was not preserved");

    racebox::race_day::SetupFieldValue flat{
        .key = "flat:ride-height",
        .label = "Ride height",
        .section = "Chassis",
        .value = "5.2",
        .unit = "mm",
        .page = 0,
        .left = 0.45,
        .top = 0.2,
        .right = 0.58,
        .bottom = 0.24,
        .confidence = 100,
        .confirmed = true,
    };
    require(racebox::pdf_setup::save_values(input, flat_edited, {flat}, error),
            error.c_str());
    const auto flat_render = racebox::pdf_setup::render_page(flat_edited, 0, 800);
    require(flat_render.ok && flat_render.bgra != rendered.bgra,
            "Flat-sheet mapped value did not alter the intended rendered page");

    std::cout << "PDFium AcroForm inspect/edit/save and flat mapping tests passed\n";
    return 0;
}

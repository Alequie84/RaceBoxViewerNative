#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace racebox::credential_store {

[[nodiscard]] bool write(
    std::string_view reference,
    std::string_view secret,
    std::string* error = nullptr);
[[nodiscard]] std::optional<std::string> read(
    std::string_view reference,
    std::string* error = nullptr);
[[nodiscard]] bool erase(
    std::string_view reference,
    std::string* error = nullptr);

}  // namespace racebox::credential_store

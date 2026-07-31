#include "racebox/plot_decimation.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
    try {
        std::vector<std::array<double, 4>> samples(1'000);
        for (std::size_t index = 0; index < samples.size(); ++index) {
            samples[index] = {
                static_cast<double>(index) * 0.01,
                0.0,
                5.0,
                0.0,
            };
        }
        samples[237][0] = 50.0;
        samples[239][3] = 42.0;
        samples[241][1] = -12.0;
        samples[777][2] = 30.0;

        const auto selected = racebox::peak_preserving_indices(
            samples.size(), 100, samples.front().size(),
            [&](std::size_t sample, std::size_t channel) {
                return samples[sample][channel];
            });
        require(selected.size() <= 100, "Decimator exceeded its point budget");
        require(selected.front() == 0 && selected.back() == samples.size() - 1,
                "Decimator did not retain both endpoints");
        require(std::is_sorted(selected.begin(), selected.end()),
                "Decimator returned time-disordered indices");
        require(std::find(selected.begin(), selected.end(), 237) != selected.end(),
                "Speed spike was lost");
        require(std::find(selected.begin(), selected.end(), 239) != selected.end(),
                "Third nearby channel event was lost");
        require(std::find(selected.begin(), selected.end(), 241) != selected.end(),
                "Opposite-channel peak in the same area was lost");
        require(std::find(selected.begin(), selected.end(), 777) != selected.end(),
                "Late telemetry peak was lost");

        const auto all = racebox::peak_preserving_indices(
            std::size_t{4}, std::size_t{8}, std::size_t{1},
            [](std::size_t sample, std::size_t) {
                return static_cast<double>(sample);
            });
        require(all == std::vector<std::size_t>({0, 1, 2, 3}),
                "Small input was changed unnecessarily");

        const auto endpoints = racebox::peak_preserving_indices(
            std::size_t{20}, std::size_t{2}, std::size_t{1},
            [](std::size_t sample, std::size_t) {
                return static_cast<double>(sample);
            });
        require(endpoints == std::vector<std::size_t>({0, 19}),
                "Two-point budget did not retain endpoints");

        std::cout << "Peak-preserving plot decimation tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failure: " << exception.what() << '\n';
        return 1;
    }
}

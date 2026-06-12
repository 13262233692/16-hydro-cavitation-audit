#pragma once

#include <cstddef>
#include <cstring>
#include <cmath>
#include <vector>
#include <array>
#include <algorithm>

namespace cavitation {

class DB4Wavelet {
public:
    static constexpr size_t FILTER_LEN = 8;

    static const double* decomposition_lo() {
        static const double h[8] = {
            -0.010597401784997278,
             0.032883011666982960,
            -0.030841381835986976,
            -0.187034811718881420,
             0.027983769416859874,
             0.630581353267056300,
             0.714846583088296240,
             0.230377809308896230
        };
        return h;
    }

    static const double* decomposition_hi() {
        static const double g[8] = {
            -0.230377809308896230,
             0.714846583088296240,
            -0.630581353267056300,
            -0.027983769416859874,
             0.187034811718881420,
             0.030841381835986976,
            -0.032883011666982960,
            -0.010597401784997278
        };
        return g;
    }

    static const double* reconstruction_lo() {
        static const double h[8] = {
             0.230377809308896230,
             0.714846583088296240,
             0.630581353267056300,
            -0.027983769416859874,
            -0.187034811718881420,
             0.030841381835986976,
             0.032883011666982960,
            -0.010597401784997278
        };
        return h;
    }

    static const double* reconstruction_hi() {
        static const double g[8] = {
            -0.010597401784997278,
            -0.032883011666982960,
             0.030841381835986976,
             0.187034811718881420,
            -0.027983769416859874,
            -0.630581353267056300,
             0.714846583088296240,
            -0.230377809308896230
        };
        return g;
    }
};

inline void convolve_dwt(const double* input, size_t n,
                         const double* filter, size_t flen,
                         double* output, size_t out_n) {
    for (size_t k = 0; k < out_n; ++k) {
        double sum = 0.0;
        size_t base = k * 2;
        for (size_t j = 0; j < flen; ++j) {
            size_t idx = base + j;
            double val = (idx < n) ? input[idx] : input[idx - n];
            sum += filter[j] * val;
        }
        output[k] = sum;
    }
}

struct WPNode {
    std::vector<double> coefficients;
    size_t              level;
    size_t              band_index;
    double              energy;
};

class WPDKernel {
public:
    static constexpr size_t LEVELS     = 3;
    static constexpr size_t NUM_BANDS  = 1 << LEVELS;

    static std::array<WPNode, NUM_BANDS> decompose(const double* signal, size_t n) {
        std::array<WPNode, NUM_BANDS> bands;

        const double* lo = DB4Wavelet::decomposition_lo();
        const double* hi = DB4Wavelet::decomposition_hi();
        constexpr size_t flen = DB4Wavelet::FILTER_LEN;

        struct WorkNode {
            std::vector<double> data;
            size_t level;
            size_t band;
        };

        std::vector<WorkNode> current_level;
        current_level.push_back({std::vector<double>(signal, signal + n), 0, 0});

        std::vector<WorkNode> next_level;

        for (size_t lv = 0; lv < LEVELS; ++lv) {
            next_level.clear();
            next_level.reserve(current_level.size() * 2);

            for (auto& node : current_level) {
                size_t in_n = node.data.size();
                size_t out_n = (in_n + 1) / 2;

                std::vector<double> approx(out_n);
                std::vector<double> detail(out_n);

                convolve_dwt(node.data.data(), in_n, lo, flen, approx.data(), out_n);
                convolve_dwt(node.data.data(), in_n, hi, flen, detail.data(), out_n);

                next_level.push_back({std::move(approx), lv + 1, node.band * 2});
                next_level.push_back({std::move(detail), lv + 1, node.band * 2 + 1});
            }

            current_level = std::move(next_level);
        }

        for (size_t i = 0; i < NUM_BANDS; ++i) {
            bands[i].level       = LEVELS;
            bands[i].band_index  = i;
            bands[i].coefficients = std::move(current_level[i].data);

            double e = 0.0;
            for (double v : bands[i].coefficients) {
                e += v * v;
            }
            bands[i].energy = e;
        }

        return bands;
    }

    static std::array<double, NUM_BANDS> energy_ratios(const std::array<WPNode, NUM_BANDS>& bands) {
        std::array<double, NUM_BANDS> ratios{};
        double total = 0.0;
        for (size_t i = 0; i < NUM_BANDS; ++i) {
            total += bands[i].energy;
        }
        if (total < 1e-30) return ratios;
        for (size_t i = 0; i < NUM_BANDS; ++i) {
            ratios[i] = bands[i].energy / total;
        }
        return ratios;
    }

    static void compute_band_freq_range(uint32_t sample_rate, size_t band_idx,
                                        double& f_low, double& f_high) {
        double nyquist = sample_rate / 2.0;
        double band_width = nyquist / NUM_BANDS;
        f_low  = band_idx * band_width;
        f_high = (band_idx + 1) * band_width;
    }
};

}

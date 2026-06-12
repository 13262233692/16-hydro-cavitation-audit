#pragma once

#include <cstdio>
#include <cstddef>
#include <cmath>
#include <vector>
#include <array>
#include "wpd_kernel.h"

namespace cavitation {

struct ChunkEnergyRow {
    size_t chunk_idx;
    uint64_t first_ts_ns;
    uint64_t last_ts_ns;
    std::array<double, WPDKernel::NUM_BANDS> ratios;
    std::array<double, WPDKernel::NUM_BANDS> energies;
};

class EnergyMatrix {
public:
    void add_row(ChunkEnergyRow row) {
        rows_.push_back(std::move(row));
    }

    size_t row_count() const { return rows_.size(); }

    const std::vector<ChunkEnergyRow>& rows() const { return rows_; }

    std::array<double, WPDKernel::NUM_BANDS> global_ratios() const {
        std::array<double, WPDKernel::NUM_BANDS> total_e{};
        total_e.fill(0.0);
        for (const auto& r : rows_) {
            for (size_t b = 0; b < WPDKernel::NUM_BANDS; ++b) {
                total_e[b] += r.energies[b];
            }
        }
        double grand = 0.0;
        for (double e : total_e) grand += e;
        std::array<double, WPDKernel::NUM_BANDS> ratios{};
        if (grand < 1e-30) return ratios;
        for (size_t b = 0; b < WPDKernel::NUM_BANDS; ++b) {
            ratios[b] = total_e[b] / grand;
        }
        return ratios;
    }

    void print_summary(uint32_t sample_rate) const {
        std::printf("\n");
        std::printf("╔══════════════════════════════════════════════════════════════════════════════╗\n");
        std::printf("║         HYDRO CAVITATION AUDIT — WPD Energy Ratio Matrix Summary           ║\n");
        std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
        std::printf("║  Wavelet : Daubechies db4  |  Levels : 3  |  Sub-bands : 8                 ║\n");

        double nyquist = sample_rate / 2.0;
        double bw = nyquist / WPDKernel::NUM_BANDS;
        std::printf("║  Fs      : %-8u Hz       |  Nyquist : %.1f kHz                        ║\n",
                     sample_rate, nyquist / 1000.0);
        std::printf("║  Chunks  : %-8zu                                                  ║\n", rows_.size());
        std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");

        std::printf("║  Band │  Freq Range (kHz)  │  Global Ratio %%  │  Total Energy (V²·s)     ║\n");
        std::printf("╠───────┼────────────────────┼──────────────────┼──────────────────────────╣\n");

        auto g_ratios = global_ratios();
        std::array<double, WPDKernel::NUM_BANDS> total_e{};
        total_e.fill(0.0);
        for (const auto& r : rows_) {
            for (size_t b = 0; b < WPDKernel::NUM_BANDS; ++b) {
                total_e[b] += r.energies[b];
            }
        }

        for (size_t b = 0; b < WPDKernel::NUM_BANDS; ++b) {
            double f_lo = b * bw / 1000.0;
            double f_hi = (b + 1) * bw / 1000.0;
            std::printf("║  S[%zu]  │ %7.1f — %-7.1f  │    %8.4f %%     │  %20.6e  ║\n",
                         b, f_lo, f_hi, g_ratios[b] * 100.0, total_e[b]);
        }

        std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
        std::printf("║  Per-Chunk Energy Ratio Matrix (%%):                                         ║\n");
        std::printf("╠───────┬────────────");

        for (size_t b = 0; b < WPDKernel::NUM_BANDS; ++b) {
            std::printf("┬───────────");
        }
        std::printf("╣\n");

        std::printf("║ Chunk │  Timestamp  ");
        for (size_t b = 0; b < WPDKernel::NUM_BANDS; ++b) {
            std::printf("│  S[%zu]     ", b);
        }
        std::printf("║\n");

        std::printf("╠───────┼────────────");
        for (size_t b = 0; b < WPDKernel::NUM_BANDS; ++b) {
            std::printf("┼───────────");
        }
        std::printf("╣\n");

        for (const auto& r : rows_) {
            double ts_sec = static_cast<double>(r.first_ts_ns) / 1e9;
            std::printf("║ %5zu │ %9.3fs ", r.chunk_idx, ts_sec);
            for (size_t b = 0; b < WPDKernel::NUM_BANDS; ++b) {
                std::printf("│ %7.3f%% ", r.ratios[b] * 100.0);
            }
            std::printf("║\n");
        }

        std::printf("╚───────┴────────────");
        for (size_t b = 0; b < WPDKernel::NUM_BANDS; ++b) {
            std::printf("┴───────────");
        }
        std::printf("╝\n");

        size_t max_band = 0;
        double max_ratio = g_ratios[0];
        for (size_t b = 1; b < WPDKernel::NUM_BANDS; ++b) {
            if (g_ratios[b] > max_ratio) {
                max_ratio = g_ratios[b];
                max_band = b;
            }
        }

        std::printf("\n");
        std::printf("  ► Dominant sub-band : S[%zu] (%.1f — %.1f kHz, %.4f%% energy)\n",
                     max_band, max_band * bw / 1000.0, (max_band + 1) * bw / 1000.0,
                     max_ratio * 100.0);

        double cav_band_energy = 0.0;
        double total_energy = 0.0;
        for (size_t b = 0; b < WPDKernel::NUM_BANDS; ++b) {
            total_energy += total_e[b];
            double f_center = (b + 0.5) * bw;
            if (f_center >= 20000.0 && f_center <= 100000.0) {
                cav_band_energy += total_e[b];
            }
        }

        if (total_energy > 1e-30) {
            double cav_pct = cav_band_energy / total_energy * 100.0;
            std::printf("  ► Cavitation band (20-100 kHz) energy share : %.4f%%\n", cav_pct);
            if (cav_pct > 15.0) {
                std::printf("  ► ⚠ WARNING: Cavitation energy ratio exceeds 15%% threshold — inspect turbine blades!\n");
            } else {
                std::printf("  ► ✓ Cavitation energy ratio within normal range.\n");
            }
        }
        std::printf("\n");
    }

private:
    std::vector<ChunkEnergyRow> rows_;
};

}

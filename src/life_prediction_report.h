#pragma once

#include <cstdio>
#include <cstdint>
#include <string>
#include <algorithm>
#include "palmgren_miner.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#endif

namespace cavitation {

#ifdef _WIN32
inline void enable_windows_ansi() {
    static bool enabled = false;
    if (enabled) return;
    DWORD mode = 0;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode)) {
        SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    enabled = true;
}

#define ANSI_RED     "\033[31m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_BLUE    "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_RESET   "\033[0m"
#else
inline void enable_windows_ansi() {}
#define ANSI_RED     "\033[31m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_BLUE    "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_RESET   "\033[0m"
#endif

inline const char* life_health_color(double life_days) {
    if (life_days <= 30.0)  return ANSI_RED;
    if (life_days <= 180.0) return ANSI_YELLOW;
    if (life_days <= 365.0) return ANSI_MAGENTA;
    return ANSI_GREEN;
}

inline const char* life_risk_label(double life_days) {
    if (life_days <= 30.0)  return "CRITICAL";
    if (life_days <= 180.0) return "HIGH";
    if (life_days <= 365.0) return "MODERATE";
    if (life_days <= 730.0) return "LOW";
    return "SAFE";
}

inline void render_progress_bar(double percentage, int width = 40) {
    int filled = static_cast<int>(percentage / 100.0 * width + 0.5);
    filled = std::max(0, std::min(width, filled));

    std::printf("    [");
    for (int i = 0; i < filled; ++i) {
        if (percentage > 80.0)       std::printf("█");
        else if (percentage > 50.0)  std::printf("▓");
        else if (percentage > 20.0)  std::printf("▒");
        else                         std::printf("░");
    }
    for (int i = filled; i < width; ++i) {
        std::printf(" ");
    }
    std::printf("] %5.1f%%", percentage);
}

inline void print_residual_life_report(const PalmgrenMinerAccumulator& accumulator,
                                        uint32_t sample_rate_hz,
                                        double daily_operation_hours = 24.0) {
    enable_windows_ansi();
    const auto& s = accumulator.state();
    const auto& mat = accumulator.material();

    double life_days_damage = accumulator.residual_life_days(daily_operation_hours);
    double life_days_erosion = accumulator.life_days_from_erosion(daily_operation_hours);
    double life_days = std::min(life_days_damage, life_days_erosion);

    double damage_pct = accumulator.damage_percentage();
    double erosion_pct = accumulator.erosion_percentage();

    double health_pct = std::max(0.0, std::min(100.0, 100.0 - damage_pct));
    double erosion_remaining_pct = std::max(0.0, std::min(100.0,
        100.0 - s.surface_erosion_um / (PalmgrenMinerAccumulator::INITIAL_THICKNESS_UM *
                                          PalmgrenMinerAccumulator::MAX_EROSION_PCT / 100.0) * 100.0));

    const char* risk = life_risk_label(life_days);

    std::printf("\n");
    std::printf("╔══════════════════════════════════════════════════════════════════════════════╗\n");
    std::printf("║  TURBINE RUNNER RESIDUAL LIFE PREDICTION REPORT  (Palmgren-Miner Model)    ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");

    std::printf("║  Material       : %-60s ║\n", mat.name);
    std::printf("║  Shear Strength : %-6.1f MPa                                              ║\n", mat.shear_strength_mpa);
    std::printf("║  Hardness       : %-6.1f HB                                               ║\n", mat.hardness_hb);
    std::printf("║  Cavitation Band: %5.0f — %-5.0f kHz  (metal spalling & bubble collapse) ║\n",
                 PalmgrenMinerAccumulator::CAVITATION_BAND_LOW_KHZ,
                 PalmgrenMinerAccumulator::CAVITATION_BAND_HIGH_KHZ);
    std::printf("║  Danger Threshold : %-5.2f%%  energy ratio                                ║\n",
                 PalmgrenMinerAccumulator::DANGER_RATIO_THRESHOLD * 100.0);
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");

    std::printf("║                         CAVITATION DAMAGE METRICS                          ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");

    std::printf("║  Total exposure time       : %10.3f seconds  (%5.2f hours)            ║\n",
                 s.total_exposure_seconds, s.total_exposure_seconds / 3600.0);
    std::printf("║  Cavitation events detected: %10zu chunks above threshold               ║\n",
                 s.chunks_above_threshold);
    std::printf("║  Total cavitation energy   : %10.4e V²·s                                ║\n",
                 s.total_cavitation_energy);
    std::printf("║  Peak energy ratio         : %10.4f %%                                  ║\n",
                 s.peak_energy_ratio * 100.0);
    std::printf("║  Equivalent shear stress   : %10.4f MPa                                 ║\n",
                 s.equivalent_stress_mpa);
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");

    std::printf("║                         FATIGUE DAMAGE (Palmgren-Miner)                   ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    std::printf("║  Cumulative damage Σ(n/N)  : %12.6f                                   ║\n",
                 s.cumulative_damage);
    std::printf("║  Damage level              : ");
    render_progress_bar(damage_pct, 36);
    std::printf("  ║\n");
    std::printf("║  Blade material health     : ");
    render_progress_bar(health_pct, 36);
    std::printf("  ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");

    std::printf("║                         SURFACE EROSION (Mass Loss)                       ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    std::printf("║  Surface erosion depth     : %10.4f μm                                  ║\n",
                 s.surface_erosion_um);
    std::printf("║  Erosion / Max allowed     : ");
    render_progress_bar(erosion_pct, 36);
    std::printf("  ║\n");
    std::printf("║  Remaining erosion margin  : ");
    render_progress_bar(erosion_remaining_pct, 36);
    std::printf("  ║\n");
    std::printf("║  Total mass loss           : %10.4f g                                    ║\n",
                 s.mass_loss_grams);
    std::printf("║  Mass loss rate            : %10.4f g/hour                               ║\n",
                 s.total_exposure_seconds > 1e-6 ?
                 s.mass_loss_grams / (s.total_exposure_seconds / 3600.0) : 0.0);
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");

    std::printf("║                   RESIDUAL SAFE SERVICE LIFE PREDICTION                   ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    std::printf("║  Daily operation assumed  : %-5.1f hours/day                              ║\n",
                 daily_operation_hours);
    std::printf("║  Life by fatigue damage   : %10.1f days  (%6.2f years)               ║\n",
                 life_days_damage, life_days_damage / 365.25);
    std::printf("║  Life by erosion limit    : %10.1f days  (%6.2f years)               ║\n",
                 life_days_erosion, life_days_erosion / 365.25);
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");

    std::printf("║  PREDICTED RESIDUAL LIFE  : ");
    if (life_days >= 10000.0) {
        std::printf("%sBEST CASE — > 27 years (safe operation)%s", ANSI_GREEN, ANSI_RESET);
    } else if (life_days >= 3650.0) {
        std::printf("%s%.1f days  (%.1f years)%s",
                     life_health_color(life_days), life_days, life_days / 365.25, ANSI_RESET);
    } else if (life_days >= 365.0) {
        std::printf("%s%.1f days  (%.1f years) — %s%s",
                     life_health_color(life_days),
                     life_days, life_days / 365.25, risk, ANSI_RESET);
    } else {
        std::printf("%s%.1f days  — %s — INSPECT IMMEDIATELY%s",
                     life_health_color(life_days), life_days, risk, ANSI_RESET);
    }
    std::printf("  ║\n");

    std::printf("║  Risk Level               : %s%-10s%s                                    ║\n",
                 life_health_color(life_days), risk, ANSI_RESET);
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");

    if (life_days <= 30.0) {
        std::printf("║  ⚠ CRITICAL ALERT: Cavitation damage is approaching failure threshold.   ║\n");
        std::printf("║    Immediate inspection and blade repair recommended.                     ║\n");
    } else if (life_days <= 180.0) {
        std::printf("║  ⚠ WARNING: High cavitation erosion rate. Schedule maintenance soon.    ║\n");
        std::printf("║    Consider reducing load or performing surface coating.                  ║\n");
    } else if (life_days <= 365.0) {
        std::printf("║  ⓘ CAUTION: Moderate wear detected. Plan annual inspection.             ║\n");
    } else {
        std::printf("║  ✓ Turbine runner condition is within safe operating range.              ║\n");
        std::printf("║    Continue normal monitoring schedule.                                   ║\n");
    }

    std::printf("╚══════════════════════════════════════════════════════════════════════════════╝\n");
    std::printf("\n");
    std::printf("  Note: Prediction based on Palmgren-Miner linear damage rule with %s\n", mat.name);
    std::printf("        S-N curve interpolation. Results are physics-phenomenological estimates.\n");
    std::printf("        Actual service life may vary with water quality, sediment content,\n");
    std::printf("        operating load variations, and maintenance history.\n");
    std::printf("\n");
}

#undef ANSI_RED
#undef ANSI_GREEN
#undef ANSI_YELLOW
#undef ANSI_BLUE
#undef ANSI_MAGENTA
#undef ANSI_CYAN
#undef ANSI_BOLD
#undef ANSI_RESET

}

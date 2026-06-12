#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <string>
#include <array>
#include <vector>
#include <algorithm>

#include "wpd_kernel.h"

namespace cavitation {

struct MaterialFatiguePoint {
    double stress_mpa;
    double cycles_to_failure;
};

struct MaterialProperties {
    const char* name;
    double shear_strength_mpa;
    double density_kg_m3;
    double young_modulus_gpa;
    double poisson_ratio;
    double hardness_hb;
    std::array<MaterialFatiguePoint, 8> sn_curve;
    size_t sn_points;
};

inline const MaterialProperties& get_default_material() {
    static const MaterialProperties mat = {
        "0Cr13Ni5Mo Martensitic Stainless Steel (Hydro Turbine Runner)",
        850.0,
        7750.0,
        200.0,
        0.30,
        260.0,
        {{
            {900.0, 1.0e3},
            {800.0, 1.0e4},
            {700.0, 5.0e4},
            {600.0, 2.0e5},
            {500.0, 1.0e6},
            {400.0, 1.0e7},
            {300.0, 1.0e8},
            {200.0, 1.0e9}
        }},
        8
    };
    return mat;
}

inline double interpolate_sn_curve(double stress_mpa, const MaterialProperties& mat) {
    if (mat.sn_points == 0) return 1.0e9;

    if (stress_mpa >= mat.sn_curve[0].stress_mpa) {
        return mat.sn_curve[0].cycles_to_failure;
    }
    if (stress_mpa <= mat.sn_curve[mat.sn_points - 1].stress_mpa) {
        return mat.sn_curve[mat.sn_points - 1].cycles_to_failure;
    }

    for (size_t i = 0; i < mat.sn_points - 1; ++i) {
        double s1 = mat.sn_curve[i].stress_mpa;
        double s2 = mat.sn_curve[i + 1].stress_mpa;
        double n1 = mat.sn_curve[i].cycles_to_failure;
        double n2 = mat.sn_curve[i + 1].cycles_to_failure;

        if (stress_mpa >= s2 && stress_mpa <= s1) {
            double ratio = (stress_mpa - s2) / (s1 - s2);
            double log_n = std::log10(n2) + ratio * (std::log10(n1) - std::log10(n2));
            return std::pow(10.0, log_n);
        }
    }
    return mat.sn_curve[mat.sn_points - 1].cycles_to_failure;
}

struct CavitationDamageState {
    double  cumulative_damage;
    double  total_cavitation_energy;
    double  total_exposure_seconds;
    double  mass_loss_grams;
    double  surface_erosion_um;
    double  equivalent_stress_mpa;
    double  cavitation_event_count;
    double  peak_energy_ratio;
    size_t  chunks_above_threshold;
};

class PalmgrenMinerAccumulator {
public:
    static constexpr double CAVITATION_BAND_LOW_KHZ  = 120.0;
    static constexpr double CAVITATION_BAND_HIGH_KHZ = 180.0;
    static constexpr double DANGER_RATIO_THRESHOLD   = 0.08;
    static constexpr double BLADE_SURFACE_AREA_M2   = 0.025;
    static constexpr double INITIAL_THICKNESS_UM    = 15000.0;
    static constexpr double MAX_EROSION_PCT        = 15.0;

    PalmgrenMinerAccumulator() {
        reset();
    }

    void reset() {
        state_ = CavitationDamageState{};
        material_ = get_default_material();
    }

    void set_material(const MaterialProperties& mat) {
        material_ = mat;
    }

    void process_chunk(double sample_rate_hz,
                       size_t chunk_frames,
                       const std::array<double, 8>& band_energies,
                       const std::array<double, 8>& band_ratios) {
        double chunk_duration = static_cast<double>(chunk_frames) / sample_rate_hz;

        double cav_energy = 0.0;
        double total_energy = 0.0;

        for (size_t b = 0; b < 8; ++b) {
            total_energy += band_energies[b];
            double f_lo, f_hi;
            WPDKernel::compute_band_freq_range(
                static_cast<uint32_t>(sample_rate_hz), b, f_lo, f_hi);

            double overlap_lo = std::max(f_lo, CAVITATION_BAND_LOW_KHZ * 1000.0);
            double overlap_hi = std::min(f_hi, CAVITATION_BAND_HIGH_KHZ * 1000.0);
            if (overlap_hi > overlap_lo) {
                double overlap_ratio = (overlap_hi - overlap_lo) / (f_hi - f_lo);
                cav_energy += band_energies[b] * overlap_ratio;
            }
        }

        double cav_ratio = total_energy > 1e-30 ? cav_energy / total_energy : 0.0;
        bool above_threshold = cav_ratio >= DANGER_RATIO_THRESHOLD;

        state_.total_exposure_seconds += chunk_duration;
        state_.total_cavitation_energy += cav_energy;

        if (above_threshold) {
            state_.chunks_above_threshold++;
            state_.cavitation_event_count += 1.0;

            if (cav_ratio > state_.peak_energy_ratio) {
                state_.peak_energy_ratio = cav_ratio;
            }

            double pressure_amplitude_pa = energy_to_pressure(cav_energy, chunk_duration);
            double stress_mpa = pressure_to_shear_stress(pressure_amplitude_pa);

            if (stress_mpa > state_.equivalent_stress_mpa) {
                state_.equivalent_stress_mpa = stress_mpa;
            }

            double cycles = chunk_duration * (CAVITATION_BAND_LOW_KHZ * 1000.0 +
                                               CAVITATION_BAND_HIGH_KHZ * 1000.0) / 2.0;
            double cycles_to_failure = interpolate_sn_curve(stress_mpa, material_);
            double damage_increment = cycles / cycles_to_failure;

            state_.cumulative_damage += damage_increment;

            double erosion_rate_um_s = cav_energy_to_erosion_rate(cav_energy, chunk_duration);
            state_.surface_erosion_um += erosion_rate_um_s * chunk_duration;

            double volume_loss = state_.surface_erosion_um * 1e-6 * BLADE_SURFACE_AREA_M2;
            state_.mass_loss_grams = volume_loss * material_.density_kg_m3 * 1000.0;
        }
    }

    double residual_life_days(double daily_operation_hours = 24.0) const {
        if (state_.cumulative_damage < 1e-12 || state_.total_exposure_seconds < 1e-6) {
            return 36500.0;
        }

        double damage_per_second = state_.cumulative_damage / state_.total_exposure_seconds;
        double remaining_damage = 1.0 - std::min(state_.cumulative_damage, 0.9999);
        double remaining_seconds = remaining_damage / damage_per_second;

        double seconds_per_day = daily_operation_hours * 3600.0;
        return remaining_seconds / seconds_per_day;
    }

    double damage_percentage() const {
        return std::min(state_.cumulative_damage * 100.0, 99.99);
    }

    double erosion_percentage() const {
        return std::min(state_.surface_erosion_um / INITIAL_THICKNESS_UM * 100.0, 99.99);
    }

    const CavitationDamageState& state() const { return state_; }
    const MaterialProperties& material() const { return material_; }

    double life_days_from_erosion(double daily_operation_hours = 24.0) const {
        if (state_.surface_erosion_um < 1e-6 || state_.total_exposure_seconds < 1e-6) {
            return 36500.0;
        }
        double erosion_rate_um_s = state_.surface_erosion_um / state_.total_exposure_seconds;
        double max_erosion_um = INITIAL_THICKNESS_UM * MAX_EROSION_PCT / 100.0;
        double remaining_um = max_erosion_um - state_.surface_erosion_um;
        if (remaining_um < 0.0) return 0.0;
        double seconds_per_day = daily_operation_hours * 3600.0;
        return remaining_um / erosion_rate_um_s / seconds_per_day;
    }

private:
    CavitationDamageState state_{};
    MaterialProperties material_{get_default_material()};

    static double energy_to_pressure(double energy, double duration) {
        if (duration < 1e-12) return 0.0;
        double power = energy / duration;
        double intensity = power / BLADE_SURFACE_AREA_M2;
        double rho = 1000.0;
        double c = 1450.0;
        double p_rms = std::sqrt(intensity * rho * c);
        return p_rms * std::sqrt(2.0);
    }

    static double pressure_to_shear_stress(double pressure_pa) {
        double pressure_mpa = pressure_pa / 1e6;
        return pressure_mpa * 0.85;
    }

    static double cav_energy_to_erosion_rate(double energy, double duration) {
        if (duration < 1e-12) return 0.0;
        double power_density = (energy / duration) / BLADE_SURFACE_AREA_M2;
        double erosion_rate_mm_per_hour = 2.5e-10 * std::pow(power_density / 1e6, 2.5);
        double erosion_rate_um_per_sec = erosion_rate_mm_per_hour * 1000.0 / 3600.0;
        return std::max(erosion_rate_um_per_sec, 0.0);
    }
};

}

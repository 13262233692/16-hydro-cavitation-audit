#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>
#include <algorithm>

#include "ae_dag_stream.h"
#include "wpd_kernel.h"
#include "energy_matrix.h"

static void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Hydro Cavitation Audit Tool v1.0.0\n"
        "Usage: %s <input.dat> [options]\n"
        "\n"
        "Options:\n"
        "  -c, --chunk <N>   Frames per WPD chunk (default: 8192)\n"
        "  -h, --help        Show this help\n"
        "\n"
        "AE binary format (.dat):\n"
        "  Header: 64 bytes (magic 'AERW', version, sample_rate, channels, epoch_ns, ...)\n"
        "  Frames: 10 bytes each (uint64 hw_timestamp_ns + int16 amplitude_mv)\n",
        prog);
}

static void generate_test_dat(const std::string& path, uint32_t sample_rate, size_t num_frames) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) {
        std::fprintf(stderr, "ERROR: Cannot create test file: %s\n", path.c_str());
        return;
    }

    cavitation::AERawHeader hdr{};
    hdr.magic[0] = 'A'; hdr.magic[1] = 'E';
    hdr.magic[2] = 'R'; hdr.magic[3] = 'W';
    hdr.version       = 1;
    hdr.sample_rate_hz = sample_rate;
    hdr.channels      = 1;
    hdr.start_epoch_ns = 1700000000000000000ULL;
    hdr.frame_size    = sizeof(cavitation::AEFrame);

    std::fwrite(&hdr, sizeof(hdr), 1, fp);

    uint64_t base_ns = hdr.start_epoch_ns;
    double dt_ns = 1e9 / static_cast<double>(sample_rate);

    double f1 = 35000.0;
    double f2 = 80000.0;
    double f3 = 120000.0;
    double f4 = 5000.0;

    for (size_t i = 0; i < num_frames; ++i) {
        cavitation::AEFrame frame{};
        frame.hw_timestamp_ns = base_ns + static_cast<uint64_t>(i * dt_ns);

        double t = static_cast<double>(i) / sample_rate;

        double sig = 0.3 * std::sin(2.0 * 3.14159265358979323846 * f1 * t)
                   + 0.5 * std::sin(2.0 * 3.14159265358979323846 * f2 * t)
                   + 0.15 * std::sin(2.0 * 3.14159265358979323846 * f3 * t)
                   + 0.4 * std::sin(2.0 * 3.14159265358979323846 * f4 * t);

        double burst = 0.0;
        size_t period = 8192;
        size_t pos = i % period;
        if (pos > period - 64) {
            burst = 2.0 * std::sin(2.0 * 3.14159265358979323846 * 90000.0 * t)
                  * std::exp(-static_cast<double>(pos - (period - 64)) / 15.0);
        }

        double noise = 0.02 * ((std::rand() % 2000 - 1000) / 1000.0);
        double val = sig + burst + noise;

        int16_t mv = static_cast<int16_t>(std::clamp(val * 1000.0, -32768.0, 32767.0));
        frame.amplitude_mv = mv;

        std::fwrite(&frame, sizeof(frame), 1, fp);
    }

    std::fclose(fp);
    std::fprintf(stderr, "Generated test file: %s (%zu frames, %u Hz)\n",
                 path.c_str(), num_frames, sample_rate);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string input_path;
    size_t chunk_frames = 8192;
    bool generate_test = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-c" || arg == "--chunk") {
            if (i + 1 < argc) {
                chunk_frames = std::atoll(argv[++i]);
                if (chunk_frames < 64) chunk_frames = 64;
            }
        } else if (arg == "--generate-test") {
            generate_test = true;
        } else if (input_path.empty()) {
            input_path = arg;
        }
    }

    if (generate_test && !input_path.empty()) {
        generate_test_dat(input_path, 500000, 500000);
    }

    if (input_path.empty()) {
        std::fprintf(stderr, "ERROR: No input file specified.\n");
        print_usage(argv[0]);
        return 1;
    }

    auto wall_start = std::chrono::high_resolution_clock::now();

    std::fprintf(stderr, "[*] Loading AE binary log: %s\n", input_path.c_str());

    cavitation::AEDAGStream stream;
    if (!stream.load(input_path)) {
        std::fprintf(stderr, "ERROR: Failed to load/parse AE file: %s\n", input_path.c_str());
        return 1;
    }

    const auto& hdr = stream.header();
    size_t total = stream.total_frames();

    std::fprintf(stderr, "[✓] File mapped successfully (zero-copy DAG)\n");
    std::fprintf(stderr, "    Sample rate : %u Hz\n", hdr.sample_rate_hz);
    std::fprintf(stderr, "    Channels    : %u\n", hdr.channels);
    std::fprintf(stderr, "    Start epoch : %llu ns\n", (unsigned long long)hdr.start_epoch_ns);
    std::fprintf(stderr, "    Total frames: %zu (%.3f seconds)\n", total,
                 static_cast<double>(total) / hdr.sample_rate_hz);
    std::fprintf(stderr, "    Chunk size  : %zu frames (%.3f ms/chunk)\n",
                 chunk_frames,
                 static_cast<double>(chunk_frames) / hdr.sample_rate_hz * 1000.0);

    cavitation::EnergyMatrix matrix;

    std::vector<double> signal_buf(chunk_frames);

    auto decomp_start = std::chrono::high_resolution_clock::now();

    stream.stream_chunks(chunk_frames, [&](const cavitation::AEReference& ref, size_t chunk_idx) {
        if (ref.count < cavitation::DB4Wavelet::FILTER_LEN) return;

        signal_buf.resize(ref.count);
        stream.extract_voltages(ref, signal_buf.data());

        auto bands = cavitation::WPDKernel::decompose(signal_buf.data(), ref.count);
        auto ratios = cavitation::WPDKernel::energy_ratios(bands);

        cavitation::ChunkEnergyRow row{};
        row.chunk_idx  = chunk_idx;
        row.first_ts_ns = ref.ptr[0].hw_timestamp_ns;
        row.last_ts_ns  = ref.ptr[ref.count - 1].hw_timestamp_ns;
        row.ratios      = ratios;
        for (size_t b = 0; b < cavitation::WPDKernel::NUM_BANDS; ++b) {
            row.energies[b] = bands[b].energy;
        }
        matrix.add_row(std::move(row));

        if (chunk_idx % 50 == 0) {
            std::fprintf(stderr, "    Processed chunk %zu / ~%zu\r",
                         chunk_idx, total / chunk_frames);
        }
    });

    auto decomp_end = std::chrono::high_resolution_clock::now();
    double decomp_ms = std::chrono::duration<double, std::milli>(decomp_end - decomp_start).count();

    std::fprintf(stderr, "\n[✓] WPD decomposition complete\n");
    std::fprintf(stderr, "    Chunks processed: %zu\n", matrix.row_count());
    std::fprintf(stderr, "    Decomposition time: %.2f ms\n", decomp_ms);
    std::fprintf(stderr, "    Throughput: %.1f kFrames/s\n",
                 static_cast<double>(total) / decomp_ms);

    matrix.print_summary(hdr.sample_rate_hz);

    auto wall_end = std::chrono::high_resolution_clock::now();
    double wall_ms = std::chrono::duration<double, std::milli>(wall_end - wall_start).count();
    std::fprintf(stderr, "[✓] Total wall time: %.2f ms\n", wall_ms);

    return 0;
}

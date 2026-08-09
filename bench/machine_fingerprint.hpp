#pragma once

// Records what the machine was when a result was produced.
//
// A latency number without a fingerprint is not a result, it is a rumour. Six
// months later the only way to know whether 42ns was good is to know what it
// was measured on, with which compiler, at which optimisation level. All of
// that is written into the header of every results file.

#include <fstream>
#include <string>
#include <thread>

#include "bench/rdtsc.hpp"

namespace matchbook::bench {

struct MachineFingerprint {
    std::string cpu_model      = "unknown";
    std::string kernel         = "unknown";
    std::string compiler       = "unknown";
    std::string build_type     = "unknown";
    std::string build_flags    = "unknown";
    unsigned    hardware_threads = 0;
    double      tsc_ghz        = 0.0;
    std::string timestamp_utc;

    [[nodiscard]] std::string to_text() const {
        std::string s;
        s += "# machine fingerprint\n";
        s += "# cpu_model        : " + cpu_model + "\n";
        s += "# hardware_threads : " + std::to_string(hardware_threads) + "\n";
        s += "# kernel           : " + kernel + "\n";
        s += "# compiler         : " + compiler + "\n";
        s += "# build_type       : " + build_type + "\n";
        s += "# build_flags      : " + build_flags + "\n";
        s += "# tsc_ghz          : " + std::to_string(tsc_ghz) + "\n";
        s += "# timestamp_utc    : " + timestamp_utc + "\n";
        s += "# NOTE: laptop frequency scaling makes absolute figures uncertain.\n";
        s += "#       Relative deltas between versions in the same run are the\n";
        s += "#       trustworthy result.\n";
        return s;
    }
};

// Reads /proc/cpuinfo and /proc/version. Both are Linux-only; on any other
// platform the fields stay "unknown" rather than the build failing, since the
// harness is documented as a WSL2/Linux tool.
[[nodiscard]] inline MachineFingerprint capture_fingerprint() {
    MachineFingerprint fp;

    fp.hardware_threads = std::thread::hardware_concurrency();
    fp.tsc_ghz = tsc_ghz();

    {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("model name", 0) == 0) {
                const auto colon = line.find(':');
                if (colon != std::string::npos && colon + 2 <= line.size()) {
                    fp.cpu_model = line.substr(colon + 2);
                }
                break;
            }
        }
    }
    {
        std::ifstream f("/proc/version");
        std::getline(f, fp.kernel);
    }

#if defined(__GNUC__) && !defined(__clang__)
    fp.compiler = "g++ " + std::to_string(__GNUC__) + "." +
                  std::to_string(__GNUC_MINOR__) + "." +
                  std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(__clang__)
    fp.compiler = "clang++ " + std::string(__clang_version__);
#endif

#ifdef MATCHBOOK_BUILD_TYPE
    fp.build_type = MATCHBOOK_BUILD_TYPE;
#endif
#ifdef MATCHBOOK_BUILD_FLAGS
    fp.build_flags = MATCHBOOK_BUILD_FLAGS;
#endif

    {
        const std::time_t now = std::time(nullptr);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
        fp.timestamp_utc = buf;
    }

    return fp;
}

}  // namespace matchbook::bench

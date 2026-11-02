// SPDX-License-Identifier: GPL-2.0-only
#include "muvm.hpp"

#include <cmath>
#include <fstream>

Muvm::Muvm(
    const std::string& drm_node, const std::string& pci_dev,
    uint16_t vendor_id, uint16_t device_id
) : GPU(drm_node, pci_dev, vendor_id, device_id, "gpu-muvm") {}

void Muvm::pre_poll_overrides() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto now = std::chrono::steady_clock::now();

    // The HUD polls many times a second, the helper answers within 200 ms: an answer's numbers stand until the next
    // one, for 3 s at most, and then nothing rather than old numbers.
    const std::string answer = dir + "/readings." + std::to_string(getpid()) + "." + std::to_string(seq);
    std::ifstream in(answer);
    if (in) {
        readings.clear();
        std::string key;
        uint64_t value;

        while (in >> key >> value)
            readings[key] = value;
        in.close();
        fs::remove(answer, ec);
        answered = now;
        seq++;
    } else if (now - answered > std::chrono::seconds(3)) {
        readings.clear();
    }

    // At the helper's pace
    if (now - asked >= std::chrono::milliseconds(200)) {
        fs::create_directories(dir, ec);
        std::ofstream(dir + "/request") << getpid() << ' ' << seq << '\n';
        asked = now;
    }
}

int Muvm::get_load() {
    return readings["gpu_busy"];
}

float Muvm::get_vram_used() {
    return readings["vram"] / (1024.f * 1024.f * 1024.f);
}

int Muvm::get_temperature() {
    return std::round(readings["gpu_temp"] / 1000.f);
}

int Muvm::get_apu_cpu_temp() {
    return std::round(readings["cpu_temp"] / 1000.f);
}

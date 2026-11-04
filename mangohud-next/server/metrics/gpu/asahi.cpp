// SPDX-License-Identifier: GPL-2.0-only
#include "asahi.hpp"
#include <cmath>
#include <mutex>

Asahi::Asahi(
    const std::string& drm_node, const std::string& pci_dev,
    uint16_t vendor_id, uint16_t device_id
) : GPU(drm_node, pci_dev, vendor_id, device_id, "gpu-asahi"), FDInfo(drm_node) {
    hwmon.base_dir = hwmon.find_hwmon_dir_by_name("macsmc_hwmon");
    hwmon.setup(sensors, drm_node);
}

void Asahi::pre_poll_overrides() {
    hwmon.poll_sensors();
    fdinfo.poll_all();
}

int Asahi::get_temperature() {
    return std::round(hwmon.get_sensor_value("temp") / 1000.f);
}

int Asahi::get_apu_cpu_temp() {
    return std::round(hwmon.get_sensor_value("cpu_temp") / 1000.f);
}

int Asahi::get_process_load(pid_t pid) {
    uint64_t* previous_gpu_time = &previous_gpu_times[pid];

    // The vertex time overlaps the fragment time of the same frame: the fragment and compute engines alone.
    uint64_t gpu_time_now = fdinfo.get_gpu_time(pid, "drm-engine-fragment") +
                            fdinfo.get_gpu_time(pid, "drm-engine-compute");

    if (!*previous_gpu_time) {
        *previous_gpu_time = gpu_time_now;
        return 0;
    }

    float delta_gpu_time = gpu_time_now - *previous_gpu_time;
    float result = delta_gpu_time / delta_time_ns.count() * 100;

    if (result > 100.f)
        result = 100.f;

    *previous_gpu_time = gpu_time_now;

    return std::round(result);
}

// The memory is shared with the CPU, so the driver keeps no total: what the
// processes the HUD follows have allocated.
float Asahi::get_vram_used() {
    float total = 0.f;

    std::lock_guard lock(fdinfo.pids_mutex);
    for (const auto& [pid, p] : fdinfo.pids)
        total += fdinfo.get_memory_used(pid, "drm-total-memory");

    return total;
}

float Asahi::get_process_vram_used(pid_t pid) {
    return fdinfo.get_memory_used(pid, "drm-total-memory");
}

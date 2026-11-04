// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>

#include "gpu.hpp"
#include "hwmon.hpp"
#include "fdinfo.hpp"

// The Apple silicon GPU (the asahi driver): the load and memory from DRM fdinfo, the GPU's and the CPU's temperatures
// from the SMC's sensors, which name them by label. The GPU shares the die with the CPU, so it reports as an APU and
// gives the CPU row its temperature.
class Asahi : public GPU, private Hwmon, public FDInfo {
private:
    const std::vector<hwmon_sensor> sensors = {
        { "temp",     "temp\\d+_input", "GPU Temperature" },
        { "cpu_temp", "temp\\d+_input", "CPU Temperature" }
    };

    std::map<pid_t, uint64_t> previous_gpu_times;

protected:
    void pre_poll_overrides() override;

public:
    Asahi(
        const std::string& drm_node, const std::string& pci_dev,
        uint16_t vendor_id, uint16_t device_id
    );

    // System-related functions
    float get_vram_used()   override;
    int  get_temperature()  override;
    bool get_is_apu()       override { return true; }
    int  get_apu_cpu_temp() override;

    // Process-related functions
    int     get_process_load(pid_t pid)         override;
    float   get_process_vram_used(pid_t pid)    override;
};

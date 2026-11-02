// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <filesystem>
#include <fstream>
#include <pwd.h>
#include <unistd.h>

#ifndef MUVM_DIR_ONLY
#include "gpu.hpp"
#endif

// muvm's guests see a virtio GPU and none of the host's sensors. The host's mangohud-muvm answers a guest's HUD through
// this directory, which the guest shares with the host (the home): the HUD writes `request`, which starts the helper
// (mangohud-muvm.path) if it is not running. A request is `PID SEQ`; the helper answers it with `readings.PID.SEQ`
// (`key value` lines: gpu_busy in percent, vram in bytes, gpu_temp and cpu_temp in millidegrees), which the HUD reads
// and deletes, and numbers its next request on. A file of its own for each answer: through virtiofs the guest keeps
// opening a file replaced under the same name for its entry timeout, and a new name is looked up afresh. The helper
// also keeps a copy of the host's system batteries in `power_supply`.
inline std::string muvm_dir() {
    const char* home = getenv("HOME");
    if (!home || !*home) {
        const passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "";
    }
    return std::string(home) + "/.cache/mangohud/muvm";
}

// muvm mounts the host's filesystem on /run/muvm-host in its guests. Proton's container (pressure-vessel) hides it but
// not the kernel's command line, which names libkrun's init.
inline bool in_muvm() {
    std::error_code ec;
    if (std::filesystem::is_directory("/run/muvm-host", ec))
        return true;
    std::string cmdline;
    std::getline(std::ifstream("/proc/cmdline"), cmdline);
    return cmdline.find(" init=/init.krun") != std::string::npos;
}

#ifndef MUVM_DIR_ONLY
class Muvm : public GPU {
private:
    const std::string dir = muvm_dir();
    std::map<std::string, uint64_t> readings;
    uint64_t seq = 1; // the request whose answer is awaited
    std::chrono::steady_clock::time_point asked, answered;

protected:
    void pre_poll_overrides() override;

public:
    Muvm(
        const std::string& drm_node, const std::string& pci_dev,
        uint16_t vendor_id, uint16_t device_id
    );

    // System-related functions: the whole VM's, which is the guest's GPU work
    int   get_load()          override;
    float get_vram_used()     override;
    int   get_temperature()   override;
    bool  get_is_apu()        override { return true; }
    int   get_apu_cpu_temp()  override;

    // Process-related functions: the VM's too, as the host cannot tell the guest's processes apart
    int   get_process_load(pid_t pid)       override { return get_load(); }
    float get_process_vram_used(pid_t pid)  override { return get_vram_used(); }
};
#endif

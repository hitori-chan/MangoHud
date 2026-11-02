// SPDX-License-Identifier: GPL-2.0-only
// mangohud-muvm: the host's GPU load, VRAM and temperatures for MangoHud in muvm's guests, which see a virtio GPU and
// none of the host's sensors. A guest's HUD writing `request` in ~/.cache/mangohud/muvm starts it (mangohud-muvm.path);
// for each request (`PID SEQ`, read as its write closes: a game's HUD processes share the file) it writes
// `readings.PID.SEQ` there: the share of the time since the last request the VMs'
// asahi DRM files kept the GPU's fragment and compute engines busy, their GPU memory, and the SMC's GPU and CPU
// temperatures. It exits 5 s after the last request. It copies the host's system batteries into `power_supply` there once a second, for the
// guests that cannot see /run/muvm-host (Proton's container).
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#define MUVM_DIR_ONLY
#include "../mangohud-next/server/metrics/gpu/muvm.hpp"

namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

// The input of the hwmon sensor labelled `label`, or "".
static std::string hwmon_input(const std::string& label) {
    std::error_code ec;
    for (const auto& d : fs::directory_iterator("/sys/class/hwmon", ec)) {
        for (const auto& f : fs::directory_iterator(d.path(), ec)) {
            std::string name = f.path().filename();
            if (name.size() < 6 || name.compare(name.size() - 6, 6, "_label"))
                continue;
            std::string l;
            std::getline(std::ifstream(f.path()), l);
            if (l == label)
                return f.path().string().substr(0, f.path().string().size() - 6) + "_input";
        }
    }
    return "";
}

static uint64_t read_u64(const std::string& path) {
    uint64_t v = 0;
    std::ifstream(path) >> v;
    return v;
}

// The host's system batteries (type Battery, scope not Device): the files MangoHud's battery reads, each replaced whole.
static void copy_batteries(const std::string& dir) {
    static const char* const files[] = { "type", "scope", "status", "capacity", "energy_now", "energy_full",
                                         "charge_now", "charge_full", "power_now", "current_now", "voltage_now" };
    std::error_code ec;
    for (const auto& b : fs::directory_iterator("/sys/class/power_supply", ec)) {
        std::string type, scope;
        std::getline(std::ifstream(b.path() / "type"), type);
        std::getline(std::ifstream(b.path() / "scope"), scope);
        if (type != "Battery" || scope == "Device")
            continue;
        const std::string to = dir + "/power_supply/" + b.path().filename().string() + "/";
        fs::create_directories(to, ec);
        for (const char* f : files) {
            std::string v;
            if (!std::getline(std::ifstream(b.path() / f), v))
                continue;
            std::ofstream(to + f + ".tmp") << v << '\n';
            fs::rename(to + f + ".tmp", to + f, ec);
        }
    }
}

// The fdinfo of every render node the VMs (`libkrun VM`) hold: their fds listed again every 30 s or when a file has
// gone (a game's VM holds tens of thousands), and /proc walked for the VMs only when none is known or one has gone.
struct VMFiles {
    std::vector<fs::path> vms;
    std::vector<std::string> fdinfos;
    clk::time_point scanned;

    static bool is_vm(const fs::path& proc) {
        std::string comm;
        std::getline(std::ifstream(proc / "comm"), comm);
        return comm == "libkrun VM";
    }

    void scan() {
        std::error_code ec;
        bool gone = vms.empty();
        for (const auto& vm : vms)
            gone |= !is_vm(vm);
        if (gone) {
            vms.clear();
            for (const auto& p : fs::directory_iterator("/proc", ec))
                if (is_vm(p.path()))
                    vms.push_back(p.path());
        }
        fdinfos.clear();
        for (const auto& vm : vms)
            for (const auto& fd : fs::directory_iterator(vm / "fd", ec)) {
                auto target = fs::read_symlink(fd.path(), ec);
                if (!ec && target.string().rfind("/dev/dri/render", 0) == 0)
                    fdinfos.push_back((vm / "fdinfo" / fd.path().filename()).string());
            }
        scanned = clk::now();
    }

    // The engines' busy time (ns) and the memory (bytes) of the asahi files, each file once (by drm-client-id).
    bool lost = true;

    void sample(uint64_t& ns, uint64_t& mem) {
        if (lost || clk::now() - scanned > std::chrono::seconds(30))
            scan();
        lost = false;
        std::set<std::string> seen;
        ns = mem = 0;
        for (const auto& path : fdinfos) {
            std::ifstream in(path);
            if (!in) {
                lost = true;
                continue;
            }
            std::string line;
            bool asahi = false, counted = false;
            while (std::getline(in, line)) {
                std::istringstream s(line);
                std::string key, value, unit;
                s >> key >> value >> unit;
                if (key == "drm-driver:")
                    asahi = value == "asahi";
                else if (key == "drm-client-id:")
                    counted = asahi && seen.insert(value).second;
                else if (!counted)
                    continue;
                else if (key == "drm-engine-fragment:" || key == "drm-engine-compute:")
                    ns += std::stoull(value);
                else if (key == "drm-total-memory:")
                    mem += std::stoull(value) << (unit == "KiB" ? 10 : unit == "MiB" ? 20 : 0);
            }
        }
    }
};

// The request in `request` into `pending`: a battery's poll asks with its PID alone, for the copy and no answer.
static void read_request(const std::string& dir, std::map<long, uint64_t>& pending) {
    long pid = 0;
    uint64_t seq = 0;
    std::ifstream(dir + "/request") >> pid >> seq;
    if (pid > 0 && seq)
        pending[pid] = seq;
}

int main() {
    const std::string dir = muvm_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);

    int in = inotify_init1(IN_CLOEXEC);
    if (in < 0 || inotify_add_watch(in, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_ATTRIB) < 0) {
        perror("mangohud-muvm: inotify");
        return 1;
    }

    const std::string gpu_temp = hwmon_input("GPU Temperature"), cpu_temp = hwmon_input("CPU Temperature");
    VMFiles vms;
    std::map<long, std::string> answers; // each HUD's last answer, deleted if it has not read it by the next
    fs::remove(dir + "/readings", ec);   // the single answer of earlier versions
    uint64_t prev_ns = 0;
    clk::time_point prev, last, copied;
    alignas(inotify_event) char buf[4096];
    bool request = true; // the one that started us
    std::map<long, uint64_t> pending; // each HUD's latest request, read at its event, until answered
    read_request(dir, pending);

    for (;;) {
        auto now = clk::now();
        // A request is answered 200 ms after the last answer at the soonest; with none, 5 s of quiet end the helper.
        auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::milliseconds(200) - (now - last));
        if (!request || wait.count() > 0) {
            pollfd p = { in, POLLIN, 0 };
            int r = poll(&p, 1, request ? int(wait.count()) : 5000);
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0)
                return 1;
            if (r == 0) {
                if (request)
                    continue;
                for (const auto& [pid, name] : answers)
                    fs::remove(name, ec);
                return 0;
            }

            ssize_t n = read(in, buf, sizeof buf);
            if (n <= 0)
                return 1;

            // the helper's own answers' events among them
            for (char* e = buf; e < buf + n; e += sizeof(inotify_event) + reinterpret_cast<inotify_event*>(e)->len) {
                auto* ev = reinterpret_cast<inotify_event*>(e);
                if (ev->len && !strcmp(ev->name, "request")) {
                    request = true;
                    read_request(dir, pending);
                }
            }
            continue;
        }
        last = now;
        request = false;

        uint64_t ns, mem;
        vms.sample(ns, mem);
        auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - prev).count();
        int busy = 0;
        if (prev_ns && ns >= prev_ns && elapsed > 0 && now - prev < std::chrono::seconds(10))
            busy = std::min<uint64_t>(100, (ns - prev_ns) * 100 / elapsed);
        prev_ns = ns;
        prev = now;

        const uint64_t gt = read_u64(gpu_temp), ct = read_u64(cpu_temp);
        for (const auto& [pid, seq] : pending) {
            const std::string name = dir + "/readings." + std::to_string(pid) + "." + std::to_string(seq);
            std::ofstream(name + ".tmp")
                << "gpu_busy " << busy << "\nvram " << mem << "\ngpu_temp " << gt << "\ncpu_temp " << ct << '\n';
            fs::rename(name + ".tmp", name, ec);
            auto& last = answers[pid];
            if (!last.empty() && last != name)
                fs::remove(last, ec);
            last = name;
        }
        pending.clear();

        if (now - copied >= std::chrono::seconds(1)) {
            copy_batteries(dir);
            copied = now;
        }
    }
}

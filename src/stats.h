// System statistics from /proc and /sys: CPU, memory, temperatures, disks,
// network, battery and top processes. Rates (CPU load, bytes/s) are computed
// between two consecutive samples.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sysmon {

struct Mount {
    std::string path, device;
    uint64_t total = 0, used = 0;
};

struct Sensor {
    std::string name;  // "coretemp · Package id 0"
    double celsius = 0;
};

struct Process {
    int pid = 0;
    std::string name;
    double cpu = 0;  // percent of one core, like top(1)
    uint64_t rss = 0;
};

struct Snapshot {
    double cpu = -1;             // 0..1 over all cores, -1 until two samples exist
    std::vector<double> cores;   // 0..1 per core
    double load[3] = {0, 0, 0};
    int procs_running = 0, procs_total = 0;
    double uptime_s = 0;
    double freq_mhz = 0;         // average current frequency, 0 if unknown
    int cpu_count = 0;
    std::string cpu_model;

    double cpu_temp = -1;        // °C, -1 if no sensor
    std::vector<Sensor> sensors;

    uint64_t mem_total = 0, mem_available = 0, swap_total = 0, swap_free = 0;

    double net_rx = 0, net_tx = 0;          // bytes/s, physical interfaces
    double disk_read = 0, disk_write = 0;   // bytes/s, whole disks
    std::vector<Mount> mounts;

    int battery = -1;            // percent, -1 without a battery
    std::string battery_status;  // "Charging", "Discharging", "Full", ...

    std::vector<Process> top;    // only when sampled with processes

    double mem_fraction() const {
        return mem_total ? double(mem_total - mem_available) / double(mem_total) : 0;
    }
};

class Sampler {
public:
    // `root` prefixes /proc and /sys paths (tests use a fake tree).
    explicit Sampler(std::string root = "");

    // Reads everything; `processes` also ranks processes by CPU (costlier).
    Snapshot sample(double now_s, bool processes, size_t top_n = 6);

private:
    struct CpuTicks {
        uint64_t idle = 0, total = 0;
    };
    std::string path(const std::string& p) const { return root_ + p; }
    void read_cpu(Snapshot& s);
    void read_memory(Snapshot& s);
    void read_temperatures(Snapshot& s);
    void read_io(Snapshot& s, double dt);
    void read_mounts(Snapshot& s);
    void read_battery(Snapshot& s);
    void read_processes(Snapshot& s, double dt);

    std::string root_;
    std::string host_root_;  // "/host" inside a Facet container, for statvfs()
    double last_time_ = -1;
    std::vector<CpuTicks> last_cpu_;  // [0] = total, then per core
    uint64_t last_rx_ = 0, last_tx_ = 0, last_read_ = 0, last_write_ = 0;
    std::map<int, uint64_t> last_proc_ticks_;
    std::string cpu_model_;
};

// Helpers shared with the UI and tests.
std::string read_file(const std::string& path);
std::string trim(const std::string& s);

}  // namespace sysmon

// Sampler against a fake /proc and /sys tree built in a temp directory.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "stats.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

void put(const fs::path& root, const std::string& rel, const std::string& content) {
    fs::path p = root / rel;
    fs::create_directories(p.parent_path());
    std::ofstream(p) << content;
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/sysmon-test-XXXXXX";
    fs::path root = mkdtemp(tmpl);

    put(root, "proc/cpuinfo", "processor\t: 0\nmodel name\t: Test CPU @ 2.00GHz\n");
    // total, then two cores: user nice system idle iowait irq softirq steal
    put(root, "proc/stat", "cpu  100 0 100 800 0 0 0 0 0 0\ncpu0 50 0 50 400 0 0 0 0 0 0\n"
                           "cpu1 50 0 50 400 0 0 0 0 0 0\nintr 1\n");
    put(root, "proc/loadavg", "0.50 0.40 0.30 2/345 999\n");
    put(root, "proc/uptime", "90061.5 1000.0\n");
    put(root, "proc/meminfo", "MemTotal:        8000000 kB\nMemFree:  1 kB\nMemAvailable:    2000000 kB\n"
                              "SwapTotal:       1000 kB\nSwapFree:        250 kB\n");
    put(root, "proc/net/dev", "Inter-|   Receive\n face |bytes\n"
                              "    lo: 999 0 0 0 0 0 0 0 999 0 0 0 0 0 0 0\n"
                              "  eth0: 1000 0 0 0 0 0 0 0 500 0 0 0 0 0 0 0\n"
                              "docker0: 777 0 0 0 0 0 0 0 777 0 0 0 0 0 0 0\n");
    put(root, "sys/class/net/eth0/device", "");
    put(root, "proc/diskstats", "   8 0 sda 1 0 100 0 1 0 200 0 0 0 0\n   8 1 sda1 1 0 100 0 1 0 200 0 0 0 0\n"
                                "   7 0 loop0 1 0 9999 0 1 0 9999 0 0 0 0\n");
    fs::create_directories(root / "sys/block/sda");
    put(root, "sys/class/hwmon/hwmon0/name", "acpitz\n");
    put(root, "sys/class/hwmon/hwmon0/temp1_input", "40000\n");
    put(root, "sys/class/hwmon/hwmon1/name", "coretemp\n");
    put(root, "sys/class/hwmon/hwmon1/temp1_input", "55000\n");
    put(root, "sys/class/hwmon/hwmon1/temp1_label", "Package id 0\n");
    put(root, "sys/class/hwmon/hwmon1/temp2_input", "61000\n");
    put(root, "sys/class/hwmon/hwmon1/temp2_label", "Core 0\n");
    put(root, "sys/class/power_supply/AC/type", "Mains\n");
    put(root, "sys/class/power_supply/BAT0/type", "Battery\n");
    put(root, "sys/class/power_supply/BAT0/capacity", "87\n");
    put(root, "sys/class/power_supply/BAT0/status", "Charging\n");
    put(root, "proc/42/stat", "42 (my (odd) proc) S 1 42 42 0 -1 4194560 1 0 0 0 100 50 0 0 20 0 1 0 1 1000 10 0\n");
    put(root, "proc/mounts", "");

    sysmon::Sampler s(root.string());
    sysmon::Snapshot a = s.sample(100.0, true);
    check(a.cpu < 0, "no CPU load before the second sample");
    check(a.cpu_count == 2, "two cores");
    check(a.cpu_model == "Test CPU @ 2.00GHz", "cpu model");
    check(near(a.load[0], 0.5) && near(a.load[2], 0.3), "load average");
    check(a.procs_running == 2 && a.procs_total == 345, "process counts");
    check(near(a.uptime_s, 90061.5), "uptime");
    check(a.mem_total == 8000000ull * 1024 && near(a.mem_fraction(), 0.75), "memory");
    check(a.swap_total == 1000ull * 1024 && a.swap_free == 250ull * 1024, "swap");
    check(near(a.cpu_temp, 55), "CPU temperature from the coretemp package sensor");
    check(a.sensors.size() == 2, "one entry per hwmon");
    check(a.battery == 87 && a.battery_status == "Charging", "battery");

    // Two seconds later: total +400 ticks, idle +100 -> 75 % busy.
    put(root, "proc/stat", "cpu  250 0 250 900 0 0 0 0 0 0\ncpu0 150 0 150 400 0 0 0 0 0 0\n"
                           "cpu1 50 0 50 500 0 0 0 0 0 0\n");
    put(root, "proc/net/dev", "  eth0: 3000 0 0 0 0 0 0 0 1500 0 0 0 0 0 0 0\n"
                              "docker0: 999999 0 0 0 0 0 0 0 999999 0 0 0 0 0 0 0\n");
    put(root, "proc/diskstats", "   8 0 sda 1 0 140 0 1 0 280 0 0 0 0\n   8 1 sda1 1 0 9999 0 1 0 9999 0 0 0 0\n");
    put(root, "proc/42/stat", "42 (my (odd) proc) S 1 42 42 0 -1 4194560 1 0 0 0 250 100 0 0 20 0 1 0 1 1000 10 0\n");
    sysmon::Snapshot b = s.sample(102.0, true);
    check(near(b.cpu, 0.75), "total CPU load");
    check(b.cores.size() == 2 && near(b.cores[0], 1.0) && near(b.cores[1], 0.0), "per-core load");
    check(near(b.net_rx, 1000) && near(b.net_tx, 500), "network rate counts physical interfaces only");
    check(near(b.disk_read, 40 * 512 / 2.0) && near(b.disk_write, 80 * 512 / 2.0), "disk rate from whole disks");
    check(b.top.size() == 1 && b.top[0].name == "my (odd) proc" && b.top[0].pid == 42, "process name with parentheses");
    check(!b.top.empty() && near(b.top[0].cpu, 200.0 / 100 / 2.0 * 100), "process CPU percent");

    fs::remove_all(root);
    if (failures) return 1;
    std::printf("ok: stats tests passed\n");
    return 0;
}

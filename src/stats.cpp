#include "stats.h"

#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace sysmon {

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

namespace {

double read_number(const std::string& path, double fallback = -1) {
    std::string s = trim(read_file(path));
    if (s.empty()) return fallback;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    return end == s.c_str() ? fallback : v;
}

std::vector<std::string> list_dir(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        out.push_back(it->path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// The host's view of a per-process /proc file: inside a Facet container our
// own mounts and network namespace differ from the host's, but PID 1 (the
// host's init, with the system.stats permission) has the real ones.
std::string host_proc(const std::string& root, const std::string& name) {
    std::string init = root + "/proc/1/" + name;
    return access(init.c_str(), R_OK) == 0 ? init : root + "/proc/" + name;
}

// Counters only grow; a reset (interface re-created, wrap) yields 0, not a spike.
double rate(uint64_t now, uint64_t before, double dt) {
    return now >= before && dt > 0 ? double(now - before) / dt : 0;
}

// Sensors that describe the CPU package, in order of preference.
int cpu_sensor_rank(const std::string& name) {
    static const char* kNames[] = {"coretemp", "k10temp", "zenpower", "cpu_thermal", "soc_thermal", "cpu"};
    for (int i = 0; i < int(std::size(kNames)); ++i)
        if (name == kNames[i]) return i;
    return -1;
}

}  // namespace

Sampler::Sampler(std::string root) : root_(std::move(root)) {
    // In a container the host's root file system is mounted at /host.
    std::error_code ec;
    if (root_.empty() && fs::is_directory("/host/proc", ec)) host_root_ = "/host";
    std::istringstream in(read_file(path("/proc/cpuinfo")));
    std::string line;
    while (std::getline(in, line)) {
        // x86: "model name", ARM: "Model" (board) or "Hardware".
        if (starts_with(line, "model name") || starts_with(line, "Model") || starts_with(line, "Hardware")) {
            size_t colon = line.find(':');
            if (colon != std::string::npos) cpu_model_ = trim(line.substr(colon + 1));
            if (starts_with(line, "model name")) break;
        }
    }
}

Snapshot Sampler::sample(double now_s, bool processes, size_t top_n) {
    Snapshot s;
    double dt = last_time_ < 0 ? 0 : now_s - last_time_;
    s.cpu_model = cpu_model_;
    read_cpu(s);
    read_memory(s);
    read_temperatures(s);
    read_io(s, dt);
    read_mounts(s);
    read_battery(s);
    if (processes) {
        read_processes(s, dt);
        if (s.top.size() > top_n) s.top.resize(top_n);
    } else {
        last_proc_ticks_.clear();  // stale deltas would show averages over a long gap
    }
    last_time_ = now_s;
    return s;
}

void Sampler::read_cpu(Snapshot& s) {
    std::istringstream in(read_file(path("/proc/stat")));
    std::string line;
    std::vector<CpuTicks> now;
    while (std::getline(in, line)) {
        if (!starts_with(line, "cpu")) break;
        std::istringstream ls(line);
        std::string name;
        uint64_t v[10] = {};
        ls >> name;
        for (auto& x : v) ls >> x;
        // user nice system idle iowait irq softirq steal (guest is inside user)
        CpuTicks t;
        t.idle = v[3] + v[4];
        for (int i = 0; i < 8; ++i) t.total += v[i];
        now.push_back(t);
    }
    s.cpu_count = now.empty() ? 0 : int(now.size()) - 1;
    if (last_cpu_.size() == now.size()) {
        for (size_t i = 0; i < now.size(); ++i) {
            uint64_t total = now[i].total - last_cpu_[i].total, idle = now[i].idle - last_cpu_[i].idle;
            double busy = total ? std::clamp(1.0 - double(idle) / double(total), 0.0, 1.0) : 0;
            if (i == 0) s.cpu = busy;
            else s.cores.push_back(busy);
        }
    }
    last_cpu_ = now;

    std::istringstream la(read_file(path("/proc/loadavg")));
    std::string procs;
    la >> s.load[0] >> s.load[1] >> s.load[2] >> procs;
    if (size_t slash = procs.find('/'); slash != std::string::npos) {
        s.procs_running = std::atoi(procs.c_str());
        s.procs_total = std::atoi(procs.c_str() + slash + 1);
    }
    s.uptime_s = std::max(0.0, read_number(path("/proc/uptime"), 0));

    double sum = 0;
    int n = 0;
    for (int i = 0; i < s.cpu_count; ++i) {
        double khz = read_number(path("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_cur_freq"));
        if (khz > 0) sum += khz / 1000, ++n;
    }
    s.freq_mhz = n ? sum / n : 0;
}

void Sampler::read_memory(Snapshot& s) {
    std::istringstream in(read_file(path("/proc/meminfo")));
    std::string key;
    uint64_t kb;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        if (!(ls >> key >> kb)) continue;
        uint64_t bytes = kb * 1024;
        if (key == "MemTotal:") s.mem_total = bytes;
        else if (key == "MemAvailable:") s.mem_available = bytes;
        else if (key == "SwapTotal:") s.swap_total = bytes;
        else if (key == "SwapFree:") s.swap_free = bytes;
    }
}

void Sampler::read_temperatures(Snapshot& s) {
    int best_rank = 1 << 30;
    const std::string base = path("/sys/class/hwmon/");
    for (const auto& hw : list_dir(base)) {
        std::string dir = base + hw + "/";
        std::string name = trim(read_file(dir + "name"));
        if (name.empty()) continue;
        double max_c = -1, package_c = -1;
        for (const auto& f : list_dir(dir)) {
            if (!starts_with(f, "temp") || f.size() < 11 || f.compare(f.size() - 6, 6, "_input") != 0) continue;
            double milli = read_number(dir + f);
            if (milli <= -40000 || milli >= 200000) continue;  // unplugged / bogus
            double c = milli / 1000;
            max_c = std::max(max_c, c);
            std::string label = trim(read_file(dir + f.substr(0, f.size() - 6) + "_label"));
            if (label == "Package id 0" || label == "Tctl" || label == "Tdie") package_c = std::max(package_c, c);
        }
        if (max_c < 0) continue;
        s.sensors.push_back({name, max_c});
        int rank = cpu_sensor_rank(name);
        if (rank >= 0 && rank < best_rank) {
            best_rank = rank;
            s.cpu_temp = package_c >= 0 ? package_c : max_c;
        }
    }
    if (s.cpu_temp >= 0) return;
    // No CPU hwmon driver: thermal zones (e.g. ARM boards, some laptops).
    const std::string tz = path("/sys/class/thermal/");
    double fallback = -1;
    for (const auto& z : list_dir(tz)) {
        if (!starts_with(z, "thermal_zone")) continue;
        std::string type = trim(read_file(tz + z + "/type"));
        double milli = read_number(tz + z + "/temp");
        if (milli <= 0 || milli >= 200000) continue;
        if (type == "x86_pkg_temp" || type.find("cpu") != std::string::npos || type.find("soc") != std::string::npos) {
            s.cpu_temp = std::max(s.cpu_temp, milli / 1000);
        } else {
            fallback = std::max(fallback, milli / 1000);
        }
    }
    if (s.cpu_temp < 0) s.cpu_temp = fallback;
}

void Sampler::read_io(Snapshot& s, double dt) {
    // Network: physical interfaces only (they have a device link), so bridges,
    // veths and tunnels do not count the same traffic twice.
    uint64_t rx = 0, tx = 0, rx_all = 0, tx_all = 0;
    bool physical = false;
    std::istringstream in(read_file(host_proc(root_, "net/dev")));
    std::string line;
    while (std::getline(in, line)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string iface = trim(line.substr(0, colon));
        if (iface == "lo") continue;
        std::istringstream ls(line.substr(colon + 1));
        uint64_t v[9] = {};
        for (auto& x : v) ls >> x;
        rx_all += v[0], tx_all += v[8];
        std::error_code ec;
        if (fs::exists(path("/sys/class/net/" + iface + "/device"), ec)) {
            rx += v[0], tx += v[8];
            physical = true;
        }
    }
    if (!physical) rx = rx_all, tx = tx_all;
    if (dt > 0) {
        s.net_rx = rate(rx, last_rx_, dt);
        s.net_tx = rate(tx, last_tx_, dt);
    }
    last_rx_ = rx, last_tx_ = tx;

    // Disks: whole devices only (partitions would double count), no loop/ram.
    uint64_t rd = 0, wr = 0;
    std::istringstream ds(read_file(path("/proc/diskstats")));
    while (std::getline(ds, line)) {
        std::istringstream ls(line);
        unsigned major, minor;
        std::string dev;
        uint64_t v[10] = {};
        if (!(ls >> major >> minor >> dev)) continue;
        for (auto& x : v) ls >> x;
        if (starts_with(dev, "loop") || starts_with(dev, "ram") || starts_with(dev, "zram") ||
            starts_with(dev, "dm-") || starts_with(dev, "md"))
            continue;
        std::error_code ec;
        if (!fs::exists(path("/sys/block/" + dev), ec)) continue;  // a partition
        rd += v[2] * 512, wr += v[6] * 512;                         // sectors read / written
    }
    if (dt > 0) {
        s.disk_read = rate(rd, last_read_, dt);
        s.disk_write = rate(wr, last_write_, dt);
    }
    last_read_ = rd, last_write_ = wr;
}

void Sampler::read_mounts(Snapshot& s) {
    std::istringstream in(read_file(host_proc(root_, "mounts")));
    std::string line;
    std::set<std::string> seen;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string dev, dir, type;
        ls >> dev >> dir >> type;
        if (!starts_with(dev, "/dev/") || starts_with(dev, "/dev/loop") || type == "squashfs") continue;
        if (!seen.insert(dev).second) continue;  // bind mounts of the same device
        // /proc/mounts escapes spaces as \040.
        for (size_t p; (p = dir.find("\\040")) != std::string::npos;) dir.replace(p, 4, " ");
        struct statvfs st{};
        if (statvfs((root_ + host_root_ + dir).c_str(), &st) != 0 || st.f_blocks == 0) continue;
        Mount m;
        m.path = dir;
        m.device = dev.substr(5);
        m.total = uint64_t(st.f_blocks) * st.f_frsize;
        m.used = m.total - uint64_t(st.f_bfree) * st.f_frsize;
        s.mounts.push_back(m);
    }
}

void Sampler::read_battery(Snapshot& s) {
    const std::string base = path("/sys/class/power_supply/");
    for (const auto& ps : list_dir(base)) {
        if (trim(read_file(base + ps + "/type")) != "Battery") continue;
        double cap = read_number(base + ps + "/capacity");
        if (cap < 0) continue;
        s.battery = int(cap);
        s.battery_status = trim(read_file(base + ps + "/status"));
        return;
    }
}

void Sampler::read_processes(Snapshot& s, double dt) {
    static const long kTicks = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    static const long kPage = sysconf(_SC_PAGESIZE) > 0 ? sysconf(_SC_PAGESIZE) : 4096;
    std::map<int, uint64_t> ticks;
    for (const auto& entry : list_dir(path("/proc"))) {
        if (entry.empty() || entry.find_first_not_of("0123456789") != std::string::npos) continue;
        int pid = std::atoi(entry.c_str());
        std::string stat = read_file(path("/proc/" + entry + "/stat"));
        // "pid (comm) state ..." — comm may contain spaces and parentheses.
        size_t open = stat.find('('), close = stat.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close < open) continue;
        std::istringstream ls(stat.substr(close + 2));
        std::string field;
        std::vector<std::string> f;
        while (ls >> field && f.size() < 22) f.push_back(field);
        if (f.size() < 22) continue;
        // After comm: [0]=state ... [11]=utime [12]=stime ... [21]=rss (pages)
        uint64_t t = std::strtoull(f[11].c_str(), nullptr, 10) + std::strtoull(f[12].c_str(), nullptr, 10);
        ticks[pid] = t;
        auto prev = last_proc_ticks_.find(pid);
        if (prev == last_proc_ticks_.end() || dt <= 0 || t < prev->second) continue;
        Process p;
        p.pid = pid;
        p.name = stat.substr(open + 1, close - open - 1);
        p.cpu = double(t - prev->second) / double(kTicks) / dt * 100;
        p.rss = std::strtoull(f[21].c_str(), nullptr, 10) * uint64_t(kPage);
        s.top.push_back(p);
    }
    last_proc_ticks_ = std::move(ticks);
    std::sort(s.top.begin(), s.top.end(), [](const Process& a, const Process& b) {
        return a.cpu != b.cpu ? a.cpu > b.cpu : a.rss > b.rss;
    });
}

}  // namespace sysmon

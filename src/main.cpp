// sysmon: live load of the machine running Facet (CPU, memory, temperature,
// disks, network, top processes). Useful when the panel's controller doubles
// as a home server. Runs as a Facet plugin process (see facet-core docs).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <unistd.h>
#include <deque>
#include <string>
#include <vector>

#include "facet/plugin.h"
#include "i18n/i18n.h"
#include "stats.h"

using facet::Json;
using facet::sdk::Canvas;
using facet::sdk::Plugin;
using facet::sdk::Screen;

namespace {

#ifndef SYSMON_VERSION
#define SYSMON_VERSION "dev"  // set by CMake
#endif

constexpr double kSampleEvery = 2.0;  // seconds
constexpr size_t kHistory = 150;      // samples: 5 minutes

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string fixed(double v, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

// Warning levels shared by gauges and bars.
std::string tone_color(double fraction) {
    if (fraction >= 0.9) return "bad";
    if (fraction >= 0.75) return "warn";
    return "accent";
}

std::string temp_color(double c) {
    if (c >= 90) return "bad";
    if (c >= 75) return "warn";
    return "good";
}

struct Series {
    std::deque<double> v;
    void push(double x) {
        v.push_back(x);
        while (v.size() > kHistory) v.pop_front();
    }
    double max() const { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }
};

class Monitor {
public:
    explicit Monitor(Plugin& plugin) : plugin_(plugin) {}

    void tick() {
        double t = now_s();
        if (last_sample_ >= 0 && t - last_sample_ < kSampleEvery) return;
        bool procs = plugin_.visible();
        snap_ = sampler_.sample(t, procs);
        last_sample_ = t;
        if (snap_.cpu >= 0) cpu_.push(snap_.cpu);
        mem_.push(snap_.mem_fraction());
        rx_.push(snap_.net_rx);
        tx_.push(snap_.net_tx);
        refresh();
    }

    // Shown immediately: the first sample has no rates yet, so take a second
    // one shortly after instead of waiting for the full interval.
    void on_visible(bool visible) {
        if (visible) last_sample_ = now_s() - kSampleEvery + 0.6;
        refresh();
    }

    void refresh() {
        std::string tile = tr("CPU {}", {percent(std::max(0.0, snap_.cpu))}) + " · " +
                           tr("RAM {}", {percent(snap_.mem_fraction())});
        if (snap_.cpu_temp >= 0) tile += " · " + fixed(snap_.cpu_temp, 0) + "°C";
        plugin_.set_tile(tile);
        if (plugin_.visible()) plugin_.set_ui(build());
    }

private:
    std::string tr(std::string_view key) const { return plugin_.tr(key); }
    std::string tr(std::string_view key, const std::vector<std::string>& args) const { return plugin_.tr(key, args); }

    std::string percent(double fraction) const { return fixed(fraction * 100, 0) + "%"; }

    std::string bytes(double b) const {
        const char* units[] = {"{} B", "{} KB", "{} MB", "{} GB", "{} TB"};
        int u = 0;
        while (b >= 1024 && u < 4) b /= 1024, ++u;
        return tr(units[u], {fixed(b, u == 0 || b >= 100 ? 0 : 1)});
    }
    std::string rate(double bps) const { return tr("{}/s", {bytes(bps)}); }

    std::string duration(double s) const {
        long m = long(s / 60), h = m / 60, d = h / 24;
        if (d > 0) return tr("{} d {} h", {std::to_string(d), std::to_string(h % 24)});
        if (h > 0) return tr("{} h {} min", {std::to_string(h), std::to_string(m % 60)});
        return tr("{} min", {std::to_string(m)});
    }

    std::string sensor_name(const std::string& hwmon) const {
        if (hwmon == "coretemp" || hwmon == "k10temp" || hwmon == "zenpower" || hwmon == "cpu_thermal") return tr("Processor");
        if (hwmon == "nvme") return tr("NVMe SSD");
        if (hwmon == "drivetemp") return tr("Disk");
        if (hwmon == "acpitz") return tr("Mainboard (ACPI)");
        if (hwmon.rfind("pch_", 0) == 0) return tr("Chipset");
        if (hwmon.rfind("iwlwifi", 0) == 0) return tr("Wi-Fi");
        if (hwmon == "amdgpu" || hwmon == "nouveau" || hwmon == "radeon") return tr("Graphics");
        return hwmon;
    }

    // --- Drawing ---------------------------------------------------------

    // Three round gauges: CPU, memory and temperature (or the root disk).
    Canvas gauges(float w, float& height) const {
        Canvas c;
        float cell = w / 3, r = std::min(cell / 2 - 14, 62.f), cy = r + 10, stroke = std::max(8.f, r * 0.16f);
        height = cy + r + 12;
        auto gauge = [&](int i, double fraction, const std::string& value, const std::string& label,
                         const std::string& color) {
            float cx = cell * (float(i) + 0.5f);
            c.arc(cx, cy, r, stroke, -135, 270, "track");
            if (fraction > 0.005) c.arc(cx, cy, r, stroke, -135, float(270 * std::min(1.0, fraction)), color);
            c.text(cx - r, cy - r * 0.34f, 2 * r, r * 0.5f, value, r * 0.4f, "text", "center", "medium");
            c.text(cx - r, cy + r * 0.28f, 2 * r, r * 0.34f, label, std::max(12.f, r * 0.21f), "text_dim", "center");
        };
        double cpu = std::max(0.0, snap_.cpu);
        gauge(0, cpu, snap_.cpu < 0 ? "…" : percent(cpu), tr("CPU"), tone_color(cpu));
        double mem = snap_.mem_fraction();
        gauge(1, mem, percent(mem), tr("RAM"), tone_color(mem));
        if (snap_.cpu_temp >= 0) {
            gauge(2, snap_.cpu_temp / 100, fixed(snap_.cpu_temp, 0) + "°", tr("Temperature"), temp_color(snap_.cpu_temp));
        } else if (!snap_.mounts.empty()) {
            const auto& m = snap_.mounts.front();
            double f = double(m.used) / double(m.total);
            gauge(2, f, percent(f), tr("Disk"), tone_color(f));
        }
        return c;
    }

    // Line chart of one or two series, newest sample at the right edge.
    void chart(Canvas& c, float x, float y, float w, float h, const Series& a, const Series* b, double max) const {
        for (int i = 0; i <= 2; ++i) c.line(x, y + h * float(i) / 2, x + w, y + h * float(i) / 2, 1, "divider");
        auto plot = [&](const Series& s, const std::string& color, bool fill) {
            if (s.v.size() < 2 || max <= 0) return;
            float step = w / float(kHistory - 1);
            float x0 = x + w - step * float(s.v.size() - 1);
            std::vector<float> pts;
            for (size_t i = 0; i < s.v.size(); ++i) {
                float px = x0 + step * float(i);
                float py = y + h - float(std::clamp(s.v[i] / max, 0.0, 1.0)) * h;
                pts.push_back(px), pts.push_back(py);
            }
            if (fill) {
                std::vector<float> area = pts;
                area.insert(area.end(), {x + w, y + h, x0, y + h});
                c.poly(area, "track");
            }
            c.polyline(pts, 2.5f, color);
        };
        plot(a, "accent", true);
        if (b) plot(*b, "good", false);
    }

    Canvas cpu_chart(float w, float& height) const {
        Canvas c;
        height = 150;
        c.text(0, 0, w * 0.6f, 24, tr("Load, last 5 min"), 15, "text_dim");
        chart(c, 0, 32, w, height - 40, cpu_, nullptr, 1.0);
        return c;
    }

    // One bar per core.
    Canvas core_bars(float w, float& height) const {
        Canvas c;
        height = 64;
        size_t n = snap_.cores.size();
        if (n == 0) return c;
        float gap = n > 32 ? 1 : 4, bw = (w - gap * float(n - 1)) / float(n);
        for (size_t i = 0; i < n; ++i) {
            float x = float(i) * (bw + gap);
            double v = snap_.cores[i];
            c.rrect(x, 0, bw, height, std::min(4.f, bw / 3), "track");
            float fh = float(v) * height;
            if (fh >= 1) c.rrect(x, height - fh, bw, fh, std::min(4.f, bw / 3), tone_color(v));
        }
        return c;
    }

    Canvas net_chart(float w, float& height) const {
        Canvas c;
        height = 160;
        double max = std::max({rx_.max(), tx_.max(), 64.0 * 1024});
        c.circle(6, 12, 5, "accent");
        c.text(16, 0, w / 2 - 16, 24, tr("Download {}", {rate(snap_.net_rx)}), 15, "text");
        c.circle(w / 2 + 6, 12, 5, "good");
        c.text(w / 2 + 16, 0, w / 2 - 16, 24, tr("Upload {}", {rate(snap_.net_tx)}), 15, "text");
        chart(c, 0, 32, w, height - 60, rx_, &tx_, max);
        c.text(0, height - 24, w, 24, tr("Scale: {}", {rate(max)}), 13, "text_dim", "end");
        return c;
    }

    Screen build() const {
        const sysmon::Snapshot& s = snap_;
        float w = float(plugin_.content_width()), h = 0;
        Screen ui(tr("System monitor"));

        Canvas g = gauges(w, h);
        ui.canvas("gauges", h, g);

        ui.section(tr("Processor"));
        Canvas cc = cpu_chart(w, h);
        ui.canvas("cpu_chart", h, cc);
        if (!s.cores.empty()) {
            Canvas cb = core_bars(w, h);
            ui.canvas("cores", h, cb);
        }
        if (!s.cpu_model.empty()) ui.info(tr("Model"), s.cpu_model);
        ui.info(tr("Cores"), std::to_string(s.cpu_count));
        if (s.freq_mhz > 0) ui.info(tr("Frequency"), tr("{} GHz", {fixed(s.freq_mhz / 1000, 2)}));
        ui.info(tr("Load average"), fixed(s.load[0], 2) + " · " + fixed(s.load[1], 2) + " · " + fixed(s.load[2], 2),
                s.load[0] > s.cpu_count ? "warn" : "normal");
        ui.info(tr("Processes"), tr("{} running of {}", {std::to_string(s.procs_running), std::to_string(s.procs_total)}));

        ui.section(tr("Memory"));
        ui.level(tr("RAM"), s.mem_fraction(), bytes(double(s.mem_total - s.mem_available)) + " / " + bytes(double(s.mem_total)));
        if (s.swap_total > 0) {
            double used = double(s.swap_total - s.swap_free);
            ui.level(tr("Swap"), used / double(s.swap_total), bytes(used) + " / " + bytes(double(s.swap_total)));
        }

        if (!s.mounts.empty()) {
            ui.section(tr("Disks"));
            for (const auto& m : s.mounts)
                ui.level(m.path + "  (" + m.device + ")", double(m.used) / double(m.total),
                         bytes(double(m.used)) + " / " + bytes(double(m.total)));
            ui.info(tr("Read"), rate(s.disk_read));
            ui.info(tr("Write"), rate(s.disk_write));
        }

        ui.section(tr("Network"));
        Canvas nc = net_chart(w, h);
        ui.canvas("net_chart", h, nc);

        if (!s.sensors.empty()) {
            ui.section(tr("Temperatures"));
            for (const auto& t : s.sensors) {
                std::string tone = t.celsius >= 90 ? "bad" : t.celsius >= 75 ? "warn" : "normal";
                ui.info(sensor_name(t.name), fixed(t.celsius, 0) + " °C", tone);
            }
        }

        ui.section(tr("Top processes"));
        if (s.top.empty()) ui.note(tr("Measuring…"));
        for (const auto& p : s.top)
            ui.info(p.name, fixed(p.cpu, p.cpu < 10 ? 1 : 0) + "% · " + bytes(double(p.rss)), p.cpu >= 90 ? "warn" : "normal");

        ui.section(tr("System"));
        ui.info(tr("Uptime"), duration(s.uptime_s));
        if (s.battery >= 0) {
            std::string status = s.battery_status == "Charging"      ? tr("charging")
                                 : s.battery_status == "Discharging" ? tr("on battery")
                                 : s.battery_status == "Full"        ? tr("full")
                                 : s.battery_status == "Not charging" ? tr("not charging")
                                 : s.battery_status == "Unknown"     ? tr("status unknown")
                                                                     : tr("plugged in");
            ui.info(tr("Battery"), std::to_string(s.battery) + "% · " + status,
                    s.battery_status == "Discharging" && s.battery < 20 ? "warn" : "normal");
        }
        return ui;
    }

    Plugin& plugin_;
    sysmon::Sampler sampler_;
    sysmon::Snapshot snap_;
    double last_sample_ = -1;
    Series cpu_, mem_, rx_, tx_;
};

}  // namespace

int main(int argc, char** argv) {
    // `sysmon --print`: one sample as text, for checking a device from a shell.
    if (argc > 1 && std::string(argv[1]) == "--print") {
        sysmon::Sampler s;
        s.sample(now_s(), true);
        usleep(1000000);
        sysmon::Snapshot n = s.sample(now_s(), true);
        std::printf("cpu %.0f%% (%d cores, %s), load %.2f, temp %.0f C\n", n.cpu * 100, n.cpu_count,
                    n.cpu_model.c_str(), n.load[0], n.cpu_temp);
        std::printf("mem %.0f%% of %llu MB, swap %llu/%llu MB\n", n.mem_fraction() * 100,
                    (unsigned long long)(n.mem_total >> 20), (unsigned long long)((n.swap_total - n.swap_free) >> 20),
                    (unsigned long long)(n.swap_total >> 20));
        std::printf("net rx %.0f B/s tx %.0f B/s, disk r %.0f B/s w %.0f B/s\n", n.net_rx, n.net_tx, n.disk_read,
                    n.disk_write);
        for (const auto& m : n.mounts)
            std::printf("mount %s (%s) %llu/%llu MB\n", m.path.c_str(), m.device.c_str(),
                        (unsigned long long)(m.used >> 20), (unsigned long long)(m.total >> 20));
        for (const auto& t : n.sensors) std::printf("sensor %s %.1f C\n", t.name.c_str(), t.celsius);
        for (const auto& p : n.top) std::printf("proc %d %s %.1f%% %llu MB\n", p.pid, p.name.c_str(), p.cpu,
                                                (unsigned long long)(p.rss >> 20));
        if (n.battery >= 0) std::printf("battery %d%% %s\n", n.battery, n.battery_status.c_str());
        return 0;
    }

    Plugin plugin("sysmon", SYSMON_VERSION);
    sysmon::register_translations(plugin.catalog());
    Monitor app(plugin);
    plugin.on_hello = [&](const Json&) { app.tick(); };
    plugin.on_visible = [&](bool v) { app.on_visible(v); };
    plugin.on_locale = [&](const std::string&) { app.refresh(); };
    plugin.on_layout = [&](int) { app.refresh(); };
    plugin.on_tick = [&] { app.tick(); };
    return plugin.run(250);
}

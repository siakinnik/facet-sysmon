# sysmon — Facet plugin

Live load of the machine Facet runs on: useful when the panel's controller
doubles as a home server. Runs as an out-of-process plugin of
[facet-core](https://github.com/siakinnik/facet-core).

## What it shows

- **Gauges**: CPU, RAM and CPU temperature (the root disk when there is no
  temperature sensor).
- **Processor**: load chart for the last 5 minutes, a bar per core, model,
  frequency, load average, running/total processes.
- **Memory**: RAM and swap.
- **Disks**: usage of every mounted block device (bind mounts and loop/snap
  images are skipped), read/write rate of whole disks.
- **Network**: download/upload chart over physical interfaces (bridges,
  veths and VPN tunnels are not counted twice).
- **Temperatures**: every hwmon sensor (CPU, NVMe, chipset, Wi-Fi, ...).
- **Top processes** by CPU (percent of one core, like `top`) with memory.
- **System**: uptime, battery (laptops as servers).

The menu tile shows `CPU 12% · RAM 43% · 52°C`.

Needs Facet 0.4 (API 3), the `system.stats` permission (Settings > Apps) and
`background` (granted by default, keeps the tile current):
in its container it then sees the host's `/proc`, `/sys` and file systems,
read-only, like any user of the device.

Everything is read from `/proc` and `/sys` every 2 seconds; no root rights, no
daemons, no external tools. Processes are only scanned while the screen is
open. The charts use the `arc`/`poly`/`polyline` canvas ops.

```bash
sysmon --print   # one sample as text, to check a device from a shell
```

## Install

On a device that already runs [Facet](https://github.com/siakinnik/facet-core)
(prebuilt static binaries for x86_64, aarch64 and armv7):

```bash
curl -fsSL https://raw.githubusercontent.com/siakinnik/facet-core/main/scripts/get.sh | sudo bash -s -- --plugin siakinnik/facet-sysmon
```

Run the same command to update. Remove with `… | sudo bash -s -- --remove-plugin sysmon`.

## Build from source

```bash
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build
FACET_PLUGIN_PATH=$PWD/build ../facet-core/build/facet   # run with a locally built core
```

The plugin SDK comes from facet-core: a checkout in `../facet-core` (or
`-DFACET_CORE_DIR=…`) is used when present, otherwise CMake downloads it at
`FACET_CORE_REF`. `build/sysmon.plugin/` is a ready plugin directory.

## Releases

Same as the other Facet plugins: CI checks translations, versions, builds,
runs the unit tests and a protocol smoke test. To release, bump the version in
`CMakeLists.txt` (`project(VERSION)`, `SYSMON_VERSION_SUFFIX`) and
`manifest.json`, push, then Actions → Release → *Run workflow*.

## Files

```
src/stats.*      readers for /proc and /sys, rates between samples (unit-tested)
src/main.cpp     plugin glue: gauges, charts, screen, tile
src/i18n/        translations
tests/           unit tests against a fake /proc and /sys tree
```

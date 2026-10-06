# toolcal: tool resource calibration for the CPE Agent

`toolcal` reads the agent's tool catalog (JSON), runs every tool many times in its own cgroup, and measures what it really costs:

- CPU time, peak memory, peak task count, disk IO, wall time and output size;
- the CPU the tool **induces** in other daemons (CcspPandMSsp, CcspWifiSsp, CcspCrSsp, rbus and so on).

From those measurements it writes a **threshold report**: a budget and a class (light / standard / heavy) for every tool, plus a verdict.

It implements Appendix A ("Calibration method") of the *CPE Agent Tool Execution Resource Governance Specification*.

## Contents

| Path | What it is |
| --- | --- |
| `src/toolcal.c` | The framework: `list`, `run`, `report`, `hog` subcommands (C, single file) |
| `run_calibration.sh` | BusyBox-compatible wrapper: runs idle, typical and stressed conditions, then the report |
| `examples/tools.example.json` | Example RDK-B catalog (`/proc` reads, `dmcli`, logs, ping) |
| `tests/selftest.json` | Self-test catalog with deliberately misbehaving tools (CPU loop, endless output, leftover daemon, fork burst) |
| `examples/sample_report/` | A report produced from `tests/selftest.json` on a Linux host |
| `third_party/cjson/` | Vendored cJSON (MIT), used unless you build with the platform cJSON |

## Build

```sh
make                                   # host build with the vendored cJSON
make CJSON=system                      # link the platform's libcjson (RDK-B already ships it)
make CC=arm-rdk-linux-gnueabi-gcc CJSON=system \
     CFLAGS="--sysroot=$SDKTARGETSYSROOT -O2"     # cross-compile with the RDK SDK
```

Requirements: a C99 compiler, Linux, and cJSON. No other libraries are needed.

A minimal Yocto recipe:

```bitbake
SUMMARY = "Tool resource calibration for the CPE Agent"
LICENSE = "CLOSED"
SRC_URI = "file://toolcal/"
S = "${WORKDIR}/toolcal"
DEPENDS = "cjson"
EXTRA_OEMAKE = "CJSON=system"
do_install() { oe_runmake install DESTDIR=${D} PREFIX=/usr; }
```

## Quick start on a device

```sh
# 1. Check what will run
toolcal list -c tools.json

# 2. Calibrate everything under idle, typical and stressed load, then report
TYPICAL_LOAD_CMD="iperf3 -c 192.168.0.10 -t 3600" \
  run_calibration.sh -c tools.json -o /tmp/calib

# 3. Read the report
cat /tmp/calib/threshold_report.md
```

Run as **root** on a **lab device**: the cgroup backends need root, and the stressed condition deliberately loads the CPU and holds half of the RAM.

## Commands

```text
toolcal list   -c tools.json
toolcal run    -c tools.json -o <dir> [-l condition] [-n runs] [-t tool]
toolcal report -c tools.json -o <prefix> [-b basis|max] runs.csv [runs.csv ...]
toolcal hog    [--cpu N] [--mem-mb M] [--seconds S]
```

- `run` measures every tool (or just `-t tool`) and appends to `<dir>/runs.csv`. The first run's output is saved to `<dir>/outputs/<tool>.txt` so you can check the tool really worked.
- `report` combines any number of `runs.csv` files (one per condition) into the threshold report. The budget basis is the worst p95 across conditions (`-b max`, the default) or one named condition (`-b stressed`).
- `hog` creates synthetic CPU and memory load for the stressed condition on devices without `stress-ng`.

`run_calibration.sh` options:

| Option | Meaning | Default |
| --- | --- | --- |
| `-c FILE` | Tool catalog | required |
| `-o DIR` | Output directory | `./calib-<timestamp>` |
| `-n N` | Runs per tool (overrides the catalog) | from catalog |
| `-C LIST` | Conditions to run | `idle typical stressed` |
| `-T CMD` | Command that generates typical load (or `TYPICAL_LOAD_CMD`); without it, `typical` is skipped | none |
| `-m PCT` | Memory held by the stress load, % of MemTotal | 50 |
| `-s SEC` | Settle time after a load starts | 10 |
| `-b BASIS` | Budget basis: `max` or a condition name | `max` |

## Catalog format

The same file can be your runtime catalog: extra keys are ignored.

```json
{
  "settings": {
    "runs": 30, "warmup": 1, "timeout_ms": 10000, "grace_ms": 2000,
    "budget_factor": 1.5, "cgroup_mode": "auto",
    "induced_procs": ["CcspPandMSsp", "CcspWifiSsp", "CcspCrSsp", "PsmSsp", "rtrouted"]
  },
  "classes": [
    { "name": "light",    "cpu_ms": 200,  "wall_ms": 2000,  "mem_kb": 8192,  "out_kb": 16 },
    { "name": "standard", "cpu_ms": 1000, "wall_ms": 10000, "mem_kb": 32768, "out_kb": 64 },
    { "name": "heavy",    "cpu_ms": 2000, "wall_ms": 10000, "mem_kb": 65536, "out_kb": 64 }
  ],
  "tools": [
    { "name": "mem_snapshot", "plane": "triage", "class": "light", "command": ["cat", "/proc/meminfo"] },
    { "name": "top_processes", "plane": "triage", "command": "top -b -n 1 | head -n 25", "shell": true },
    { "name": "wifi_radio_stats", "plane": "triage", "class": "standard",
      "command": ["dmcli", "eRT", "getv", "Device.WiFi.Radio.1.Stats."],
      "induced_procs": ["CcspWifiSsp", "CcspCrSsp", "rtrouted"] }
  ]
}
```

Tool fields:

| Field | Meaning |
| --- | --- |
| `name` | Tool name (letters, digits, `_ - .`; anything else becomes `_`) |
| `command` | An argv array (preferred, no shell), or a string. A string without `"shell": true` is split on spaces, honouring simple quotes, the same way the runtime tokenizes. |
| `shell` | `true` runs the string through `/bin/sh -c` (needed for pipes) |
| `plane`, `class` | Optional. A declared class that differs from the measured class is flagged for review. |
| `runs`, `timeout_ms` | Per-tool overrides |
| `induced_procs` | Daemons whose CPU this tool drives; overrides the settings list |

Settings (all optional):

| Setting | Default | Meaning |
| --- | --- | --- |
| `runs` / `warmup` | 30 / 1 | Measured runs per tool; warm-up runs are recorded but excluded from statistics |
| `timeout_ms` / `grace_ms` | 10000 / 2000 | Stop a run after this long: SIGTERM to its process group, then SIGKILL after the grace period |
| `poll_ms` | 10 | Sampling interval for memory and task count when the kernel has no peak counters |
| `cooldown_ms` | 200 | Pause between runs so one run's after-effects don't land in the next |
| `save_output_kb` / `hard_output_kb` | 64 / 4096 | Output saved from run 1; output size at which a run is killed as runaway |
| `budget_factor` | 1.5 | Budget = p95 × factor |
| `min_cpu_ms`, `min_wall_ms`, `min_mem_kb`, `min_out_kb` | 10, 100, 1024, 1 | Floors so tiny tools don't get zero budgets |
| `cgroup_mode` | `auto` | `auto`, `v2`, `v1` or `none` |
| `cgroup_v2_root` / `cgroup_v1_root` | `/sys/fs/cgroup` | Where the hierarchies are mounted |
| `group` | `toolcal` | Parent cgroup name created for the runs |
| `induced_procs` | common CCSP daemons | Default daemon list for induced-CPU measurement |

## How it measures

Each run gets a fresh cgroup. The child joins it **before** `exec`, and the clock starts once it has joined, so cgroup setup cost is not counted as tool time.

| Metric | cgroup v2 | cgroup v1 | No cgroups (`rusage`) |
| --- | --- | --- | --- |
| CPU | `cpu.stat usage_usec` | `cpuacct.usage` | `wait4()` user + system |
| Peak memory | `memory.peak`, else polled `memory.current` | `memory.max_usage_in_bytes` | `ru_maxrss` (largest single process only) |
| Peak tasks | `pids.peak`, else polled `pids.current` | polled `pids.current` | not measured |
| Disk IO | `io.stat` | `blkio.throttle.io_service_bytes` | `ru_inblock` / `ru_oublock` |
| Leftover processes | `cgroup.procs` after exit (killed) | `cgroup.procs` after exit (killed) | process group after exit (killed) |

Also, per run:

- **Induced CPU:** `utime + stime` of the `induced_procs` daemons from `/proc/<pid>/stat`, before and after the run. A `dmcli` call is cheap itself but makes the CCSP component do the work; this is where that cost shows up.
- **Other CPU:** total busy CPU from `/proc/stat`, minus the tool and induced CPU. This is background noise, recorded for context only.
- **PSI:** stall time added to `/proc/pressure/cpu` and `/proc/pressure/memory` during the run, when the kernel supports PSI.

Accuracy notes:

- cgroup v1 memory includes page cache the tool touches, so it reads a little higher than v2 for file-heavy tools. That errs on the safe side.
- Induced CPU uses clock ticks (usually 10 ms), so it is meaningful for tools that drive real work, not for 1 ms tools.
- Background activity can land in induced CPU. Calibrate on a quiet lab device, and use enough runs (30 or more) that p95 is stable.

## Outputs

```text
<outdir>/device.txt                 kernel, CPU, memory, PSI and cgroup support
<outdir>/catalog.txt                what was run
<outdir>/<condition>/runs.csv       one row per run (raw data)
<outdir>/<condition>/outputs/       first-run output of each tool
<outdir>/threshold_report.md        human-readable report
<outdir>/threshold_report.json      machine-readable, with catalog-ready "budget" objects
<outdir>/threshold_report.csv       one row per tool, for spreadsheets
```

`runs.csv` columns: `tool, plane, condition, run, warmup, rc, signal, timed_out, output_capped, wall_ms, cpu_ms, rusage_cpu_ms, mem_kb, pids, read_kb, write_kb, out_bytes, induced_cpu_ms, other_cpu_ms, psi_cpu_us, psi_mem_us, leftover_procs, backend`. A value of `-1` means not measured on this backend.

### How budgets, classes and verdicts are set

1. For each tool and condition: p50, p95 and max of every metric (nearest-rank), excluding warm-up runs.
2. **Budget** = worst p95 across conditions (or the `-b` condition) × `budget_factor`, rounded up (CPU to 10 ms, wall to 100 ms, memory to 256 KB, output to 1 KB), with the floors above. **CPU budget includes induced CPU.**
3. **Class** = the first class in `classes` whose limits hold all four budgets; otherwise `unclassified`.
4. **Verdict**:
   - **FAIL**: exceeds every class, timed out, or hit the output hard cap.
   - **REVIEW**: fits a class but exited non-zero, left processes behind, gets most of its CPU from other daemons, has unstable timing (p95 > 3 × p50), or measured a different class from the one declared.
   - **PASS**: everything else.

`toolcal report` exits with 1 when any tool fails, so it can gate a CI or release pipeline.

### Using the results

Copy each tool's `budget` object and `class` from `threshold_report.json` into the runtime catalog (spec §3). Re-run on every firmware release and compare: the spec flags any tool whose p95 grows by more than 20%.

## Self-test

On any Linux machine:

```sh
make
sudo ./run_calibration.sh -c tests/selftest.json -o /tmp/selftest -s 3 -m 20 \
     -T "while :; do cat /proc/meminfo > /dev/null; done"
```

Expected: `mem_snapshot`, `proc_scan` and `fork_burst` pass; `compress_work` and `big_buffer` are flagged because they measure a bigger class than declared; `exits_nonzero` and `leaves_daemon` are flagged for review; `cpu_loop` (timeout) and `endless_output` (output cap) fail.

## Safety

- Tools run as root. Only calibrate catalogs you trust, and only on lab devices.
- The stressed condition holds `-m` percent of RAM and saturates every CPU. Lower `-m` on devices with little memory headroom.
- Every run is contained: timeout, process-group kill, output hard cap, and cleanup of anything left behind. The cgroups `toolcal` creates are removed when it finishes.

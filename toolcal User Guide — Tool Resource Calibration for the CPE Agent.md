# toolcal User Guide — Tool Resource Calibration for the CPE Agent

Oct 5, 2026 · @Vinu Palackal

## 1. Overview

**toolcal** measures what each CPE Agent tool really costs on a device, and turns those measurements into a resource budget and a class for every tool. It reads the agent's tool catalog (JSON), runs every tool many times in its own cgroup, and writes a threshold report.

For each run it records:

- CPU time, peak memory, peak task count, disk IO, wall time and output size;
- the CPU the tool **induces** in other daemons, such as CcspPandMSsp, CcspWifiSsp, CcspCrSsp and rbus.

From the results, the report gives each tool a budget, a class (light / standard / heavy) and a verdict (PASS / REVIEW / FAIL). The budgets go back into the runtime catalog, where the agent enforces them.

toolcal implements Appendix A ("Calibration method") of the CPE Agent — Tool Execution Resource Governance Specification.

Who this guide is for: platform engineers who calibrate tools on lab devices, and tool authors who need to know what budget their tool will get.

&#91;embedded content: calibration workflow · 4 steps, repeated each release\]

toolcal covers the middle two steps; the budgets it produces become the limits the agent enforces on field devices.

## 2. Package contents

Everything ships in `toolcal.zip`: one C source file, one shell wrapper, example catalogs and a vendored JSON parser.

| Path | What it is |
| --- | --- |
| `src/toolcal.c` | The framework, with the `list`, `run`, `report` and `hog` commands (C99, single file) |
| `run_calibration.sh` | BusyBox-compatible wrapper that runs the idle, typical and stressed conditions, then the report |
| `Makefile` | Host build, cross-compile and install targets |
| `examples/tools.example.json` | Example RDK-B catalog: `/proc` reads, `dmcli`, logs, ping |
| `tests/selftest.json` | Self-test catalog with deliberately misbehaving tools (CPU loop, endless output, leftover daemon, fork burst) |
| `examples/sample_report/` | A report produced from the self-test catalog on a Linux host |
| `third_party/cjson/` | Vendored cJSON (MIT licence), used unless you build against the platform cJSON |
| `README.md` | Short reference version of this guide |

## 3. Build and install

toolcal needs only a C99 compiler, Linux and cJSON. RDK-B already ships cJSON, so on a device build you link the platform copy instead of the vendored one.

```sh
make                                   # host build with the vendored cJSON
make CJSON=system                      # link the platform libcjson
make CC=arm-rdk-linux-gnueabi-gcc CJSON=system \
     CFLAGS="--sysroot=$SDKTARGETSYSROOT -O2"     # cross-compile with the RDK SDK
make install DESTDIR=<rootfs> PREFIX=/usr        # installs toolcal, run_calibration.sh, example catalog
```

A minimal Yocto recipe:

```text
SUMMARY = "Tool resource calibration for the CPE Agent"
LICENSE = "CLOSED"
SRC_URI = "file://toolcal/"
S = "${WORKDIR}/toolcal"
DEPENDS = "cjson"
EXTRA_OEMAKE = "CJSON=system"
do_install() { oe_runmake install DESTDIR=${D} PREFIX=/usr; }
```

For a quick trial without rebuilding the image, copy the cross-compiled `toolcal` and `run_calibration.sh` to `/tmp` on the device and run them from there.

## 4. Quick start on a device

Three commands take you from a catalog to a report. Run them as **root** on a **lab device**: the cgroup measurements need root, and the stressed condition deliberately loads every CPU and holds half the RAM.

1. Copy `examples/tools.example.json` to `tools.json` and edit the commands, log paths and `induced_procs` for your platform (§5).
2. Check what will run:

   ```sh
   toolcal list -c tools.json
   ```
3. Calibrate under all three conditions and build the report:

   ```sh
   TYPICAL_LOAD_CMD="iperf3 -c 192.168.0.10 -t 3600" \
     run_calibration.sh -c tools.json -o /tmp/calib
   ```
4. Read the result:

   ```sh
   cat /tmp/calib/threshold_report.md
   ```

With the default 30 runs per tool, each condition takes about 31 × (tool run time + 0.2 s) per tool: under 10 s for a quick /proc read, about 2 minutes for a 3-second ping. Use `-n 5` for a fast first pass.

## 5. Writing the tool catalog

The catalog is one JSON file with three parts: `settings`, `classes` and `tools`. Only `tools` is required. toolcal ignores keys it does not know, so the same file can also be the runtime catalog.

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
    { "name": "mem_snapshot", "plane": "triage", "class": "light",
      "command": ["cat", "/proc/meminfo"] },
    { "name": "top_processes", "plane": "triage",
      "command": "top -b -n 1 | head -n 25", "shell": true },
    { "name": "wifi_radio_stats", "plane": "triage", "class": "standard",
      "command": ["dmcli", "eRT", "getv", "Device.WiFi.Radio.1.Stats."],
      "induced_procs": ["CcspWifiSsp", "CcspCrSsp", "rtrouted"] }
  ]
}
```

### Tool fields

| Field | Required | Meaning |
| --- | --- | --- |
| `name` | Yes | Tool name. Letters, digits, `_`, `-` and `.`; anything else becomes `_` |
| `command` | Yes | An argv array (preferred: no shell), or a string. A string without `"shell": true` is split on spaces, honouring simple quotes, the same way the runtime tokenizes commands |
| `shell` | No | `true` runs the string through `/bin/sh -c`. Needed for pipes and redirection |
| `plane` | No | Shown in the report |
| `class` | No | The class you expect. If the measured class differs, the tool is flagged for review |
| `runs`, `timeout_ms` | No | Per-tool overrides of the settings |
| `induced_procs` | No | Daemons whose CPU this tool drives. Replaces the settings list for this tool |

### Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `runs` / `warmup` | 30 / 1 | Measured runs per tool. Warm-up runs are recorded but left out of the statistics |
| `timeout_ms` / `grace_ms` | 10000 / 2000 | Stop a run after this long: SIGTERM to its process group, then SIGKILL after the grace period |
| `poll_ms` | 10 | Sampling interval for memory and task count when the kernel has no peak counters |
| `cooldown_ms` | 200 | Pause between runs, so one run's after-effects don't land in the next |
| `save_output_kb` | 64 | Output saved from the first run, for checking the tool worked |
| `hard_output_kb` | 4096 | Output size at which a run is killed as runaway |
| `budget_factor` | 1.5 | Budget = p95 × this factor |
| `min_cpu_ms`, `min_wall_ms`, `min_mem_kb`, `min_out_kb` | 10, 100, 1024, 1 | Floors, so very small tools don't get zero budgets |
| `cgroup_mode` | `auto` | `auto`, `v2`, `v1` or `none` |
| `cgroup_v2_root` / `cgroup_v1_root` | `/sys/fs/cgroup` | Where the cgroup hierarchies are mounted |
| `group` | `toolcal` | Parent cgroup created for the runs and removed afterwards |
| `induced_procs` | Common CCSP daemons | Default daemon list for induced-CPU measurement |

### Classes

Classes are checked in the order listed, so list the smallest first. If you leave `classes` out, toolcal uses the light / standard / heavy limits from the specification's §3.

Tips for good catalog entries:

- Prefer an argv array. It matches how the runtime executes tools and avoids measuring a shell as well.
- Set `induced_procs` for every `dmcli` or rbus tool to the components it talks to. Otherwise most of its real cost is invisible.
- Give slow network tools such as ping fewer `runs` (10 is enough) to keep calibration time reasonable.

## 6. Command reference

`toolcal` has four commands. Most people only call `list` directly and let `run_calibration.sh` (§7) call the rest.

```text
toolcal list   -c tools.json
toolcal run    -c tools.json -o <dir> [-l condition] [-n runs] [-t tool]
toolcal report -c tools.json -o <prefix> [-b basis|max] runs.csv [runs.csv ...]
toolcal hog    [--cpu N] [--mem-mb M] [--seconds S]
```

| Command | What it does | Key options |
| --- | --- | --- |
| `list` | Prints every tool with its plane, declared class, run count and command, plus the class limits | `-c` catalog |
| `run` | Measures every tool, appending one row per run to `<dir>/runs.csv`. Saves the first run's output to `<dir>/outputs/<tool>.txt` | `-o` output dir, `-l` condition label (default `idle`), `-n` runs override, `-t` one tool only |
| `report` | Combines any number of `runs.csv` files (one per condition) into the threshold report | `-o` file prefix, `-b` budget basis: `max` (worst condition, default) or a condition name |
| `hog` | Creates synthetic load for the stressed condition on devices without `stress-ng` | `--cpu` spinners, `--mem-mb` resident memory, `--seconds` (0 = until stopped) |

While `run` works, it prints one character per run: `w` warm-up, `.` success, `x` failure (non-zero exit or timeout). After each tool it prints average CPU, maximum wall time and maximum memory.

Exit codes: `run` returns 0 when finished and 130 if interrupted. `report` returns 1 when any tool fails, so it can gate a CI or release pipeline.

## 7. Running the load conditions

`run_calibration.sh` measures every tool under three load conditions, because a tool costs more on a busy device and is often called exactly then. It runs `toolcal run` once per condition, then `toolcal report` over all of them.

| Condition | Load applied | Why it matters |
| --- | --- | --- |
| `idle` | None; device booted and settled | Baseline cost |
| `typical` | Your command from `-T` or `TYPICAL_LOAD_CMD`, such as iperf through the data path. Skipped if none is given | Cost in normal service |
| `stressed` | `stress-ng` on every CPU plus `-m`% of RAM held; falls back to `toolcal hog` when `stress-ng` is missing | Cost when the device is struggling |

```text
run_calibration.sh -c tools.json [-o DIR] [-n N] [-C "idle typical stressed"]
                   [-T CMD] [-m PCT] [-s SEC] [-b BASIS]
```

| Option | Meaning | Default |
| --- | --- | --- |
| `-c FILE` | Tool catalog | Required |
| `-o DIR` | Output directory | `./calib-<timestamp>` |
| `-n N` | Runs per tool (overrides the catalog) | From catalog |
| `-C LIST` | Conditions to run, in order | `idle typical stressed` |
| `-T CMD` | Command that generates typical load | None |
| `-m PCT` | Memory held by the stress load, as % of MemTotal | 50 |
| `-s SEC` | Settle time after a load starts, before measuring | 10 |
| `-b BASIS` | Budget basis: `max` or one condition name | `max` |

The script also writes `device.txt` (kernel, CPU, memory, PSI and cgroup support) and `catalog.txt`, so every report records what it was measured on. Load processes are started in their own session and stopped after each condition, including on Ctrl+C.

Examples:

```sh
# Only idle and stressed, 10 runs each, lighter memory stress
run_calibration.sh -c tools.json -C "idle stressed" -n 10 -m 30

# Re-measure one tool after a fix, without the wrapper
toolcal run -c tools.json -o /tmp/recheck/stressed -l stressed -t wifi_radio_stats
```

## 8. How measurement works

Each run gets a fresh cgroup, and the kernel's own counters are read after the tool exits. This captures the tool and every child process it starts, at almost no cost. The child joins its cgroup **before** `exec`, and the clock starts once it has joined, so cgroup setup time (slow on v1) is not counted as tool time.

toolcal picks the best backend the kernel offers (`cgroup_mode: auto`): v2 first, then v1, then plain process accounting.

| Metric | cgroup v2 | cgroup v1 | No cgroups (rusage) |
| --- | --- | --- | --- |
| CPU | `cpu.stat usage_usec` | `cpuacct.usage` | `wait4()` user + system time |
| Peak memory | `memory.peak`, else polled `memory.current` | `memory.max_usage_in_bytes` | `ru_maxrss`: largest single process only |
| Peak tasks | `pids.peak`, else polled `pids.current` | Polled `pids.current` | Not measured |
| Disk IO | `io.stat` | `blkio.throttle.io_service_bytes` | `ru_inblock` / `ru_oublock` |
| Leftover processes | Found in `cgroup.procs` after exit, then killed | Found in `cgroup.procs` after exit, then killed | Found in the process group after exit, then killed |

Three more measurements are taken around every run:

- **Induced CPU.** `utime + stime` of the `induced_procs` daemons, read from `/proc/<pid>/stat` before and after. A `dmcli` call is cheap itself but makes the CCSP component do the work; this is where that cost appears, and it is **added to the tool's CPU budget**.
- **Other CPU.** Total busy CPU from `/proc/stat`, minus the tool and induced CPU. This is background activity, recorded for context only.
- **PSI.** Stall time added to `/proc/pressure/cpu` and `/proc/pressure/memory` during the run, when the kernel has PSI.

Accuracy notes:

- cgroup v1 memory includes page cache the tool touches, so file-heavy tools read slightly higher than on v2. That errs on the safe side.
- Induced CPU is counted in clock ticks (usually 10 ms), so it is meaningful for tools that drive real work, not for 1 ms tools.
- Background activity can land in induced CPU. Calibrate on a quiet lab device, with 30 or more runs so p95 is stable.

## 9. Reading the report

The report gives every tool four budget numbers, a class and a verdict. Start with the summary table in `threshold_report.md`; look at a tool's detail table only when its verdict is REVIEW or FAIL.

### Output files

| File | Contents |
| --- | --- |
| `threshold_report.md` | Human-readable report: summary, per-tool detail by condition, notes, class limits |
| `threshold_report.json` | Machine-readable, with a catalog-ready `budget` object per tool |
| `threshold_report.csv` | One row per tool, for spreadsheets |
| `<condition>/runs.csv` | Raw data, one row per run |
| `<condition>/outputs/<tool>.txt` | First-run output of each tool |
| `device.txt`, `catalog.txt` | What was measured, and on what |

`runs.csv` columns: `tool, plane, condition, run, warmup, rc, signal, timed_out, output_capped, wall_ms, cpu_ms, rusage_cpu_ms, mem_kb, pids, read_kb, write_kb, out_bytes, induced_cpu_ms, other_cpu_ms, psi_cpu_us, psi_mem_us, leftover_procs, backend`. A value of `-1` means not measured on this backend.

### How budgets, classes and verdicts are set

1. **Statistics.** For each tool and condition: p50, p95 and maximum of every metric (nearest-rank), leaving out warm-up runs.
2. **Budget.** Worst p95 across conditions (or the `-b` condition) × `budget_factor`, rounded up: CPU to 10 ms, wall time to 100 ms, memory to 256 KB, output to 1 KB, never below the floors. The CPU budget includes induced CPU.
3. **Class.** The first class whose limits hold all four budgets; otherwise `unclassified`.
4. **Verdict:**
   - **FAIL**: exceeds every class, timed out, or hit the output hard cap.
   - **REVIEW**: fits a class but exited non-zero, left processes behind, got most of its CPU from other daemons, had unstable timing (p95 more than 3 × p50), or measured a different class from the one declared.
   - **PASS**: everything else.

### Example: the self-test results

This summary came from the self-test catalog on a Linux host (cgroup v1, 12 runs per tool per condition, 3 for the two runaway tools). Budgets are in ms and KB.

| Tool | What it does | Class | Verdict | CPU | Wall | Memory | Output | Why |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| mem\_snapshot | `cat /proc/meminfo` | light | PASS | 10 | 100 | 1024 | 3 | Fits light |
| proc\_scan | `ls` of `/proc` entries | light | PASS | 10 | 100 | 1536 | 1 | Fits light |
| fork\_burst | Starts 10 short background jobs | light | PASS | 30 | 500 | 4864 | 1 | Fits light; peaked at 11 tasks |
| compress\_work | gzip of 8 MB random data | standard | REVIEW | 460 | 500 | 2816 | 1 | Declared light, measured standard |
| big\_buffer | `dd` with a 40 MB buffer | heavy | REVIEW | 50 | 100 | 62208 | 1 | Declared standard, measured heavy |
| exits\_nonzero | `ls` of a missing path | light | REVIEW | 10 | 100 | 1024 | 1 | Non-zero exit in every run |
| leaves\_daemon | Starts a background sleep and exits | light | REVIEW | 10 | 100 | 1280 | 1 | Left a process behind every run (killed by toolcal) |
| cpu\_loop | Infinite shell loop | unclassified | FAIL | 4520 | 4600 | 1024 | 1 | Timed out at 3 s; CPU over heavy limit |
| endless\_output | `yes` | unclassified | FAIL | 10 | 100 | 1280 | 1656 | Hit the output hard cap |

Every tool in the example also carries the note "fewer than 20 runs", because the self-test uses short run counts on purpose.

## 10. Applying the results

The report's job ends when its budgets are in the runtime catalog, where the agent enforces them through admission control and cgroup limits.

1. **Fix FAIL tools first.** Rewrite the tool, narrow what it collects, or drop it from the catalog. A FAIL tool should never ship.
2. **Resolve REVIEW tools.** Accept the measured class or change the tool, add missing `induced_procs`, and fix tools that exit non-zero or leave processes behind.
3. **Copy budgets into the catalog.** Take each tool's `class` and `budget` from `threshold_report.json`:

   ```json
   { "name": "mem_snapshot", "class": "light",
     "budget": { "cpu_ms": 10, "wall_ms": 100, "mem_kb": 1024, "out_kb": 3 } }
   ```
4. **Mark essential tools.** Decide which LIGHT tools may run when the device is under heavy load (`"essential": true`, spec §3). toolcal does not decide this.
5. **Repeat every release.** Re-run calibration on each firmware release and keep the reports. Investigate any tool whose p95 grows by more than 20%.
6. **Compare with the field.** The agent's per-tool usage log (spec §11) shows real-device costs. Recalibrate any tool whose field usage drifts from its budget.

## 11. Troubleshooting

| Symptom | Likely cause | What to do |
| --- | --- | --- |
| `warning: not running as root; using rusage only` | Not root | Run as root. Without root, memory covers only the largest single process and task counts are missing |
| `no usable cgroup hierarchy; using rusage only` | Kernel without cgroups, or hierarchies not mounted | Mount cgroups, or accept rusage-only results. Check `device.txt` for what the device supports |
| `cgroup v2 requested but not usable` | `cgroup_mode: "v2"` set, but the v2 hierarchy has no cpu or memory controller | Set `cgroup_mode` to `auto` or `v1` |
| A tool fails every run with rc 127 | Command not found, or not on the `PATH` | Use the full path in `command`, for example `/usr/bin/dmcli` |
| A tool fails every run with rc 126 | Could not join its cgroup | Check that `/sys/fs/cgroup` is writable by root and that no other `toolcal` instance is running |
| `pids` column is `-1` | No pids controller on this backend | Expected on rusage-only devices; task count is not measured |
| Induced CPU is always 0 for a `dmcli` tool | `induced_procs` names don't match the daemon names | Check names with `cat /proc/<pid>/comm`; only the first 15 characters are compared |
| Budgets vary a lot between runs | Background activity on the device | Calibrate on a quiet device, raise `runs`, and check `other_cpu_ms` in `runs.csv` |
| Output file shows an error message | The tool "succeeded" but did something unexpected | Check `<condition>/outputs/<tool>.txt` before trusting a PASS |
| `report` exits with code 1 | At least one tool failed | Intended; see the FAIL rows in the report |
| `tool "x" not found in catalog` | `-t` name doesn't match | Names are sanitised; check them with `toolcal list` |

## 12. Safety and self-test

toolcal runs real commands as root and deliberately loads the device, so use it only on lab devices with catalogs you trust.

- **Trusted catalogs only.** Every `command` runs as root. Review a catalog with `toolcal list` before running it.
- **Stress load.** The stressed condition saturates every CPU and holds `-m` percent of RAM (default 50). Lower `-m` on devices with little memory headroom.
- **Containment.** Every run has a timeout, a process-group kill and an output hard cap. Anything a tool leaves behind is killed. The cgroups toolcal creates are removed when it finishes, and load processes are stopped after each condition, including on Ctrl+C.
- **No production use.** toolcal is a lab tool. Field devices are governed by the agent's own runtime limits, using the budgets toolcal produced.

### Self-test

The self-test confirms toolcal works on a machine before you trust it on a device. On any Linux machine:

```sh
make
sudo ./run_calibration.sh -c tests/selftest.json -o /tmp/selftest -s 3 -m 20 \
     -T "while :; do cat /proc/meminfo > /dev/null; done"
```

Expected result (the §9 example table):

- [ ] `mem_snapshot`, `proc_scan` and `fork_burst` pass
- [ ] `compress_work` and `big_buffer` are flagged for review: each measures a bigger class than declared
- [ ] `exits_nonzero` and `leaves_daemon` are flagged for review
- [ ] `cpu_loop` fails on timeout and `endless_output` fails on the output cap
- [ ] No `toolcal` cgroups and no stray processes remain afterwards

The self-test was run on a Linux host with cgroup v1 and with no cgroups. The cgroup v2 path compiles but has not yet been tested on hardware, so run the self-test on your first v2 device.

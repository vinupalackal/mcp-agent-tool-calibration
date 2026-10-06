# CPE Agent — Tool Execution Resource Governance Specification

Oct 5, 2026 · @Vinu Palackal

## 1. Purpose and scope

Health-metric tools run on the same CPU, memory and flash as the gateway's core services. This spec defines how the CPE Agent keeps every tool inside a fixed resource budget, refuses or delays work when the device is under pressure, runs only one tool at a time, and accepts at most 10 tools per request.

| ID | Requirement | Enforced in |
| --- | --- | --- |
| R1 | Each tool stays within a bounded CPU, memory, IO and output budget and never degrades core services (data path, CCSP components, Wi-Fi) | §3, §5, §6 |
| R2 | Calls that arrive while the device is under pressure are handled safely: run, defer or deny by tool class | §2, §4, §9 |
| R3 | At most one tool executes on the device at any moment | §7 |
| R4 | A single request names at most 10 tools | §8 |

In scope: every tool the runtime manager runs, whether from a catalog, a native or script plugin, or pushed from the cloud. Out of scope: what a tool collects, and cloud-side scheduling.

Guiding rule: fail closed, degrade gracefully. A tool that cannot run safely returns a structured status; it never runs uncontained.

All numeric values are proposed defaults, to be calibrated on each target platform (§12, §14).

## 2. Pressure model

The agent rates the device GREEN, AMBER or RED once per second, and every admission and abort decision in this spec reads that rating. The rating is the worst of the signals below.

| Signal | Source | AMBER at | RED at |
| --- | --- | --- | --- |
| CPU pressure | `/proc/pressure/cpu` some avg10 | ≥ 20% | ≥ 40% |
| Memory pressure | `/proc/pressure/memory` avg10 | some ≥ 10% | full ≥ 5% or some ≥ 25% |
| IO pressure | `/proc/pressure/io` some avg10 | ≥ 20% | ≥ 40% |
| Available memory | `MemAvailable` in `/proc/meminfo` | < 20% of total | < 10% of total |
| CPU busy (fallback when PSI is absent) | `/proc/stat`, 5 s window | ≥ 70% | ≥ 90% |
| Free disk on tmp and log partitions | `statvfs()` | < 15% | < 5% |
| Task count | `/proc/loadavg` total tasks | ≥ 75% of `max_tasks` | ≥ 90% of `max_tasks` |

Rules:

- **Escalate at once.** The level rises the first time any signal crosses its threshold.
- **Recover slowly.** The level drops one step only after every signal has stayed at least 20% below that step's thresholds for 10 s. This stops the agent flapping between levels.
- **Sampling cost.** Each sample reads a handful of `/proc` files, well under 1 ms; the agent never spawns a process to measure.
- **Kernel support.** PSI needs Linux 4.20+ with `CONFIG_PSI=y`. On older RDK-B kernels the CPU-busy and memory rows stand in for PSI, and the IO signal is skipped.

## 3. Tool classes and budgets

Every catalog and plugin entry declares a class and a budget; an entry that declares neither is treated as HEAVY with the HEAVY defaults. The budget is what the kernel enforces in §5 and what the watchdog checks in §6.

| Class | Typical tools | CPU time | Wall time | Peak memory | Output |
| --- | --- | --- | --- | --- | --- |
| LIGHT | Read `/proc` or `/sys`, a single data-model parameter | 200 ms | 2 s | 8 MB | 16 KB |
| STANDARD | Multi-parameter dumps, log tail, short ping | 1 s | 10 s | 32 MB | 64 KB |
| HEAVY | Log bundles, Wi-Fi scan, packet capture, speed test | 2 s | 10 s | 64 MB | 64 KB |

No class may exceed the existing ceilings of 10 s wall time and 64 KB output.

**Essential flag.** A LIGHT tool that collects pressure evidence (memory snapshot, top processes, PSI values) can be marked `essential`. Essential tools still run in RED, at half their budget, because health data is most needed exactly when the device is struggling.

Catalog entry example:

```json
{
  "name": "mem_snapshot",
  "plane": "triage",
  "class": "light",
  "essential": true,
  "budget": { "cpu_ms": 200, "wall_ms": 2000, "mem_kb": 8192, "out_kb": 16 },
  "cache_ttl_s": 30,
  "min_interval_s": 5
}
```

**Calibration.** Each tool is measured on reference hardware, idle and under the §13 stress load, using the method in Appendix A. Its budget is its p95 usage × 1.5. A tool whose p95 exceeds its class limits moves up a class, or fails certification.

## 4. Admission control

Right before each tool starts, the agent checks the current pressure level against the tool's class and returns RUN, DEFER or DENY. The decision is made per tool, not per request, so in a 10-tool request some tools can run while others are deferred or denied.

| Tool class | GREEN | AMBER | RED |
| --- | --- | --- | --- |
| LIGHT, essential | Run | Run | Run at half budget |
| LIGHT | Run | Run | Defer |
| STANDARD | Run | Defer | Deny |
| HEAVY | Run | Deny | Deny |

- **Fit check, at every level.** The tool's memory budget must be below `MemAvailable` minus a reserve (`mem_reserve`, default 15% of total). A tool that writes to disk needs free space of twice its output budget plus a reserve. If either check fails, the tool is deferred.
- **Defer.** The tool waits in the queue, re-checked once per second, for up to `defer_max_wait` (default 20 s). If it is still not admitted, it returns `DEFERRED_TIMEOUT` with a `retry_after_s` hint.
- **Deny.** The agent returns `RESOURCE_PRESSURE` at once, with the current level and the signal that caused it, so the cloud can tell the device is struggling.
- **Cloud timeout.** `defer_max_wait` plus the tool's wall time must stay below the cloud's WRP request timeout, otherwise the cloud gives up before the answer arrives (§14).

## 5. Execution containment

Each tool runs as its own child process inside a dedicated cgroup, at the lowest scheduling priority, with hard kernel limits. An overrun is clipped by the kernel as it happens, not discovered afterwards.

| Control | Mechanism | Setting |
| --- | --- | --- |
| CPU share | cgroup v2 `cpu.weight` on the tools slice | 10 (core services default to 100), so tools mostly get idle CPU |
| CPU ceiling | `cpu.max` per tool | 20% of one core (`20000 100000`) |
| Memory hard cap | `memory.max` | The tool's memory budget |
| Memory soft cap | `memory.high` | 80% of the hard cap: the kernel throttles and reclaims before it reaches OOM |
| Swap | `memory.swap.max` | 0 |
| Task count | `pids.max` | 16, which stops runaway forks |
| IO | `io.weight`, plus `ionice -c3` | 10 / idle class |
| Scheduling | `SCHED_IDLE` (fallback: `nice 19`) | Runs only when the CPU would otherwise be idle |
| Backup limits | `setrlimit()` in the child before `execve` | `RLIMIT_CPU` = CPU budget + 1 s, `RLIMIT_AS` = 2 × memory budget, `RLIMIT_NPROC` = 16, `RLIMIT_FSIZE` = output budget, `RLIMIT_CORE` = 0 |
| OOM order | `oom_score_adj` | 1000 for tool processes, so the kernel kills a tool before any core service |
| Process group | `setsid()` | A stop signal reaches the tool and all of its children |
| Launch | Tokenized `execve`, no shell (existing) | Close inherited fds, minimal env, run as an unprivileged user where the tool allows |

**Integration on RDK-B.** The agent's systemd unit gets `Delegate=yes` and a `cpe-agent-tools.slice` with `CPUWeight=10`, `MemoryMax`, `TasksMax` and `IOWeight=10`. The agent creates one child cgroup per tool run and removes it after reaping.

**Fallbacks.** On cgroup v1, use `cpu.shares=102`, `memory.limit_in_bytes` and the `pids` controller. With no cgroups at all, the rlimits and `SCHED_IDLE` still apply, and the HEAVY class is disabled.

## 6. In-flight monitoring and timeouts

While a tool runs, a watchdog reads its cgroup counters (`cpu.stat`, `memory.current`, `memory.events`) every 250 ms, along with the device pressure level. It stops the tool as soon as any rule below fires.

| Trigger | Action | Status returned |
| --- | --- | --- |
| Wall time exceeds the budget (never more than 10 s) | `SIGTERM` to the process group, 2 s grace, then `SIGKILL` | `TIMEOUT` |
| CPU time exceeds the budget | Same stop sequence | `BUDGET_EXCEEDED` (cpu) |
| `memory.events` reports `oom_kill` | The kernel already killed it; the agent reports | `BUDGET_EXCEEDED` (memory) |
| Output exceeds the budget | Stop reading, keep what was captured, stop the tool | `OK_TRUNCATED` |
| Device rises to RED while a non-essential tool is running | Same stop sequence | `ABORTED_PRESSURE` |
| CPU busy but no new output for `no_progress_timeout` (default 5 s) | Treated as a loop; same stop sequence | `HUNG` |

**Deadline margin.** A tool starts only if the request's remaining time is at least its wall budget plus `deadline_margin` (default 1 s). Otherwise it is skipped with `SKIPPED_DEADLINE`, so a tool is never started just to be killed.

**Cleanup, every time.** Reap the child with `waitpid`, remove its cgroup directory, free its buffers, release the execution slot (§7). Partial output is returned with its status.

## 7. Concurrency: one tool at a time

Across the whole device, exactly one tool executes at any moment (`max_concurrent_tools = 1`). Every other request waits in a bounded first-in, first-out queue.

&#91;embedded content: request path · one execution slot, bounded queue\]

A request passes validation once, then each of its tools takes the single slot in turn, passes admission, runs contained and adds its result to the response.

- **Replace thread-per-request.** Today each request gets its own detached worker thread. Instead, the receive loop hands validated requests to one executor worker that owns the slot.
- **The slot.** A semaphore initialised to 1, held from `fork` until the child is reaped and its cgroup removed. In-process `.so` plugin calls take the same slot.
- **Order.** Tools in a request run one after another, in the order listed.
- **Queue limit.** At most `queue_depth` requests wait (default 4), and at most 1 per requester. A request that finds the queue full gets `BUSY` with `retry_after_s`.
- **Pressure diagnostics first.** In AMBER or RED, a request containing only essential tools goes to the head of the queue, so pressure evidence is not stuck behind slow work.
- **Time in the queue counts.** Waiting time comes out of the request deadline (§8).

## 8. Request limits

A request may name at most 10 tools. The agent checks this while decoding, before anything is queued or run, and rejects the whole request if it is exceeded.

1. **Tool count.** `tools[]` must hold 1 to `max_tools_per_request` entries (default 10). An empty list returns `REQUEST_INVALID`; 11 or more returns `TOO_MANY_TOOLS`. Nothing runs in either case.
2. **Duplicates.** Repeated entries (same tool, same arguments) are rejected as `REQUEST_INVALID`, so a request can't repeat a tool to multiply its load.
3. **HEAVY limit.** At most 1 HEAVY tool per request; a second one is rejected up front.
4. **Per-tool checks.** A tool that is unknown, not allowed in the request's plane, or blocked by the safety gate gets its own `NOT_FOUND`, `NOT_PERMITTED` or `BLOCKED` status. The rest of the request still runs.
5. **Request deadline.** A request has `request_deadline` from receipt (default 60 s), including time in the queue. Tools that cannot start in time (§6 deadline margin) return `SKIPPED_DEADLINE`.
6. **Response size.** Each tool keeps its 64 KB output cap, and the whole response is capped at `max_response_kb` (default 256 KB). Once the cap is reached, the remaining tools return `OUTPUT_LIMIT` without running.
7. **Payload size.** The incoming request payload is capped at 16 KB; anything larger is dropped as `REQUEST_INVALID`.

## 9. Result caching and rate limiting

Repeated calls are answered from a short-lived cache and capped by rate limits. Bursts of cloud requests are most likely when a device misbehaves, which is exactly when it can least afford extra load.

- **Result cache.** Results are cached per tool and argument set for the tool's `cache_ttl_s` (default 0 for STANDARD and HEAVY, 30 s for LIGHT). The TTL doubles in AMBER and quadruples in RED. Cached replies carry `cached: true` and `age_s`, so the cloud knows how fresh the data is.
- **Per-tool cooldown.** A tool does not run again within `min_interval_s` of its last run (default 5 s LIGHT, 30 s STANDARD, 300 s HEAVY). Inside the cooldown, the cached result is returned if there is one; otherwise `RATE_LIMITED`.
- **Per-requester rate.** A token bucket allows 6 requests a minute, with bursts of up to 3.
- **Device duty cycle.** Total tool CPU time is capped at 5% of one core over a rolling 5-minute window. Above that, STANDARD and HEAVY tools are deferred until usage drops, even in GREEN.

## 10. Status codes and response contract

Every tool in a request gets its own status, and the request carries the pressure level at the time of the reply, so the cloud can always tell a refused call from a failed one.

| Status | Scope | Meaning | Cloud should retry? |
| --- | --- | --- | --- |
| `OK` | Tool | Ran within budget | No |
| `OK_TRUNCATED` | Tool | Ran; output cut at its cap | No |
| `OK_CACHED` | Tool | Served from cache; see `age_s` | No |
| `TOO_MANY_TOOLS` | Request | More than 10 tools; nothing ran | Yes, split it |
| `REQUEST_INVALID` | Request | Empty list, duplicates, second HEAVY tool, payload too large | No, fix the request |
| `BUSY` | Request | Queue full | Yes, after `retry_after_s` |
| `RATE_LIMITED` | Request or tool | Requester rate or tool cooldown hit | Yes, after `retry_after_s` |
| `NOT_FOUND` / `NOT_PERMITTED` / `BLOCKED` | Tool | Unknown, wrong plane, or on the destructive-command list | No |
| `RESOURCE_PRESSURE` | Tool | Denied by admission control | Yes, after `retry_after_s` |
| `DEFERRED_TIMEOUT` | Tool | Deferred, never admitted | Yes, after `retry_after_s` |
| `TIMEOUT` / `HUNG` | Tool | Ran past its wall time, or looped with no output | Investigate the tool |
| `BUDGET_EXCEEDED` | Tool | Went over its CPU or memory budget | Investigate the tool |
| `ABORTED_PRESSURE` | Tool | Stopped because the device rose to RED | Yes, after `retry_after_s` |
| `SKIPPED_DEADLINE` / `OUTPUT_LIMIT` | Tool | Request deadline or response size reached | Yes, in a new request |
| `INTERNAL_ERROR` | Either | Agent fault | Yes, once |

Per-tool fields: `tool`, `status`, `exit_code`, `output`, `truncated`, `cached`, `age_s`, and `usage` = `{cpu_ms, wall_ms, mem_peak_kb}`.

Request fields: `request_status`, `pressure_level` (`GREEN` / `AMBER` / `RED`), `pressure_signal` (the worst signal), and `retry_after_s` when any tool can be retried.

## 11. Observability

Every tool run writes one log line with its measured usage, read from its cgroup before the cgroup is removed. Operators can check each tool's real cost against its budget over time.

```text
TOOL_RUN req=7f3a tool=mem_snapshot class=light level=AMBER status=OK cpu_ms=41 wall_ms=180 mem_peak_kb=2304 out_kb=3 queued_ms=420
```

The agent also:

- logs every pressure-level change, with the signal and value that caused it;
- keeps counters for runs, each status code, queue depth (current and peak), time spent in each pressure level, and budget overruns per tool;
- reports its own CPU and memory use, so the agent's overhead is visible too;
- writes logs to a RAM ring buffer, rate-limited to 10 lines a second, to avoid wearing out flash or adding IO under pressure.

Tools that hit `BUDGET_EXCEEDED`, `TIMEOUT` or `HUNG` 3 times in 24 hours are flagged for recalibration in the startup and runtime tool inventory.

## 12. Configuration parameters

All limits live in the agent's config file, can be overridden by environment variables, and reload on `SIGHUP` without a restart. The values below are proposed starting points, except the two marked as existing.

| Parameter | Default | Section |
| --- | --- | --- |
| `max_concurrent_tools` | 1 | §7 |
| `queue_depth` | 4 requests | §7 |
| `max_queued_per_requester` | 1 | §7 |
| `max_tools_per_request` | 10 | §8 |
| `max_heavy_per_request` | 1 | §8 |
| `request_deadline` | 60 s | §8 |
| `max_response_kb` | 256 KB | §8 |
| `exec_timeout_max` | 10 s (existing) | §3, §6 |
| `max_output_kb` | 64 KB (existing) | §3, §6 |
| `deadline_margin` | 1 s | §6 |
| `no_progress_timeout` | 5 s | §6 |
| `term_grace` | 2 s | §6 |
| `watchdog_interval` | 250 ms | §6 |
| `pressure_sample_interval` | 1 s | §2 |
| `pressure_recover_dwell` | 10 s | §2 |
| `pressure_recover_margin` | 20% | §2 |
| AMBER / RED thresholds | See §2 table | §2 |
| `mem_reserve` | 15% of total memory | §4 |
| `defer_max_wait` | 20 s | §4 |
| `tools_cpu_weight` | 10 | §5 |
| `tool_cpu_max` | 20% of one core | §5 |
| `tool_pids_max` | 16 | §5 |
| `duty_cycle_cpu` | 5% over 5 min | §9 |
| `requester_rate` | 6 per minute, burst 3 | §9 |

## 13. Verification and acceptance

The spec is met when every scenario below passes on each target platform with the core-service checks holding. Load is generated with `stress-ng` and purpose-built misbehaving test tools.

| Scenario | How | Pass when |
| --- | --- | --- |
| Normal load | Run each catalog tool 100 times at idle | p95 usage within budget; data-path throughput drops ≤ 2% while tools run |
| CPU pressure | `stress-ng --cpu <cores>` until RED | STANDARD and HEAVY denied; essential LIGHT tools still answer |
| Memory pressure | Allocate until `MemAvailable` < 10% | No non-tool process is OOM-killed; tools denied or deferred |
| Escalation mid-run | Push the device to RED while a STANDARD tool runs | Tool stopped within 1 s with `ABORTED_PRESSURE` |
| CPU loop | Test tool spins forever | Stopped at its wall or CPU budget; status `TIMEOUT` or `HUNG` |
| Fork bomb | Test tool forks repeatedly | Capped at 16 tasks by `pids.max`; whole group killed |
| Memory hog | Test tool allocates past its budget | Killed inside its cgroup; status `BUDGET_EXCEEDED` |
| Endless output | Test tool writes forever | Output cut at 64 KB; status `OK_TRUNCATED` |
| 11-tool request | Send 11 tools | `TOO_MANY_TOOLS`; nothing runs |
| Parallel requests | Send 6 requests at once | 1 runs, 4 queue, 1 gets `BUSY`; never 2 tools running together |
| Request burst | 20 identical requests in 10 s | Rate limits and cache hold tool runs to the cooldown |
| Older kernel | Platform without PSI or cgroup v2 | Fallback signals and limits engage; HEAVY disabled with no cgroups |
| Keepalive under load | Queue full and device in RED | Keepalive acks still sent on time |

Core-service checks, measured during every scenario: no core process restarts, no OOM kill outside the tools cgroup, Parodus stays connected, and the agent's own memory stays flat.

## 14. Open questions

- [ ] **Resource limits in the current code.** The runtime already denies or defers tools when CPU, memory, disk or process limits are exceeded, but those values are not in the documents. Look up the actual limits in the code (config struct, `#define`s or config file) and either use them in the §2 and §12 tables, or record why the proposed values replace them.
- [ ] **Cloud's WRP request timeout.** `defer_max_wait` (how long one tool waits for the device to calm down, proposed 20 s) and `request_deadline` (total time for the whole request, including queue and deferral, proposed 60 s) must both be shorter than the time the cloud waits for a reply. Otherwise the cloud gives up and the device's answer is wasted. Confirm the cloud's timeout and adjust both values.
- [ ] **Meaning of "margin policy".** The runtime's option list mentions "timeouts and margin policies for tool calls". This spec assumes it means the deadline margin in §6: a tool starts only if the time left is at least its maximum run time plus 1 s. Check the code and confirm, or say what it does instead. Other possible meanings: a grace period added to each timeout, a resource headroom kept free after a tool runs, or a gap between device and cloud timeouts.
- [ ] **Parodus message size.** What is the largest message Parodus accepts? This sets `max_response_kb`.
- [ ] **Kernel support per platform.** Which kernel version, PSI support and cgroup version does each target platform ship?
- [ ] **Essential tools.** Which tools are marked essential (allowed in RED), and who signs off on that list?
- [ ] **Deferred replies.** Should a deferred tool reply right away with `DEFERRED` and deliver its result later, instead of holding the request open?

## Appendix A. Calibration method

Each tool's budget comes from running it in its own cgroup and reading the kernel's counters after it exits. This captures the tool and every child process exactly, at almost no cost; `top` or `ps` sampling misses short spikes and children, and is not used.

### A.1 Metrics per run

| Metric | Source (cgroup v2) | Fallback |
| --- | --- | --- |
| CPU time | `cpu.stat` → `usage_usec` | `wait4()` rusage `ru_utime + ru_stime` |
| Peak memory | `memory.peak` (kernel 5.19+) | cgroup v1 `memory.max_usage_in_bytes`, or poll `memory.current` |
| Processes spawned | `pids.peak` | Count forks in an `strace -f` run |
| Disk IO | `io.stat` → `rbytes`, `wbytes` | `/proc/<pid>/io` |
| Wall time | Timestamps before and after | `/usr/bin/time -v` |
| Output size | Bytes captured | `wc -c` |

`ru_maxrss` reports only the largest single process, not the total across children, so it is not used for memory budgets.

### A.2 Measurement harness

Runs one command in a fresh cgroup and prints one CSV line: label, exit code, wall ms, CPU ms, peak memory KB, peak tasks, read and written bytes, output bytes. It uses only BusyBox-compatible commands; `/proc/uptime` gives 10 ms resolution.

```sh
#!/bin/sh
# usage: measure_tool.sh <label> <command> [args...]
LABEL=$1; shift
ROOT=/sys/fs/cgroup/toolbench
mkdir -p $ROOT
echo "+cpu +memory +pids +io" > /sys/fs/cgroup/cgroup.subtree_control
echo "+cpu +memory +pids +io" > $ROOT/cgroup.subtree_control
CG=$ROOT/$LABEL.$$; mkdir $CG

t0=$(cut -d' ' -f1 /proc/uptime)
sh -c "echo \$\$ > $CG/cgroup.procs; exec \"\$@\"" sh "$@" > /tmp/tool.out 2>&1
rc=$?
t1=$(cut -d' ' -f1 /proc/uptime)

cpu_ms=$(awk '/usage_usec/{print int($2/1000)}' $CG/cpu.stat)
mem_kb=$(( $(cat $CG/memory.peak 2>/dev/null || echo 0) / 1024 ))
pids=$(cat $CG/pids.peak 2>/dev/null || echo NA)
io=$(awk '{for(i=2;i<=NF;i++){split($i,a,"=");if(a[1]=="rbytes")r+=a[2];if(a[1]=="wbytes")w+=a[2]}}END{print r+0","w+0}' $CG/io.stat)
out=$(wc -c < /tmp/tool.out)
wall_ms=$(awk "BEGIN{print int(($t1-$t0)*1000)}")

echo "$LABEL,$rc,$wall_ms,$cpu_ms,$mem_kb,$pids,$io,$out"
rmdir $CG
```

### A.3 Induced cost in other processes

Many tools are cheap themselves but make another process do the work. For example, `dmcli eRT getv Device.WiFi.` triggers work in CcspWifiSsp, CcspCr and rbus, outside the tool's cgroup. For every tool:

- read `utime + stime` from `/proc/<pid>/stat` for each component the tool talks to (P&M, Wi-Fi agent, CR, rbus), before and after the run;
- read total CPU from `/proc/stat` before and after, and subtract the tool's own CPU to get the induced CPU;
- add the induced CPU to the tool's CPU budget for classification and admission. A tool that looks LIGHT on its own may be STANDARD once induced cost is counted.

### A.4 Test conditions

Run each tool 30 to 100 times in each condition:

1. **Idle:** device booted and settled, no traffic. This is the baseline.
2. **Typical:** realistic traffic, such as iperf through the data path plus connected Wi-Fi clients.
3. **Stressed:** `stress-ng --cpu <cores> --vm 1 --vm-bytes <50% of RAM>`, because tools are often called when the device is struggling.

Record the first run after boot separately; cold caches can make it much slower.

### A.5 Impact on the device

While each tool runs, also record:

- **PSI change:** `/proc/pressure/cpu` and `/proc/pressure/memory` avg10, before versus during the run;
- **Data-path throughput:** iperf with and without the tool running; target ≤ 2% drop;
- **Management latency:** time of a simple `dmcli` get during the run, to show whether the tool slows the data model for other callers.

### A.6 From measurements to budgets

1. Take **p95** of the stressed runs, and note the maximum.
2. Budget = p95 × 1.5, rounded up (§3).
3. Choose the class (LIGHT / STANDARD / HEAVY) from the budget **including induced cost**.
4. Re-run the harness on every firmware release; flag any tool whose p95 grows by more than 20%.
5. Compare against field data from the per-tool usage log (§11) and adjust budgets that drift.

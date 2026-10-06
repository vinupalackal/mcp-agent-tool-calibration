# Tool threshold calibration report

9 tools measured: **3 pass, 4 need review, 2 fail**. Budgets are p95 x 1.50 of the worst condition, with CPU including time induced in other daemons.

- Generated: 2026-10-05T22:26:25-0400
- Host: vm, kernel 6.18.44-fc-v70
- Conditions: idle, typical, stressed
- Input: `/tmp/claude-0/cal/idle/runs.csv`, `/tmp/claude-0/cal/typical/runs.csv`, `/tmp/claude-0/cal/stressed/runs.csv`

## Summary

| Tool | Plane | Class | Verdict | CPU budget (ms) | Wall budget (ms) | Memory budget (KB) | Output budget (KB) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| mem_snapshot | triage | light | PASS | 10 | 100 | 1024 | 3 |
| proc_scan | triage | light | PASS | 10 | 100 | 1536 | 1 |
| compress_work | triage | standard | REVIEW | 460 | 500 | 2816 | 1 |
| big_buffer | triage | heavy | REVIEW | 50 | 100 | 62208 | 1 |
| fork_burst | triage | light | PASS | 30 | 500 | 4864 | 1 |
| exits_nonzero | triage | light | REVIEW | 10 | 100 | 1024 | 1 |
| leaves_daemon | triage | light | REVIEW | 10 | 100 | 1280 | 1 |
| cpu_loop | triage | unclassified | FAIL | 4520 | 4600 | 1024 | 1 |
| endless_output | triage | unclassified | FAIL | 10 | 100 | 1280 | 1656 |

## Per-tool detail

Values are p50 / p95 / max over the measured runs; warm-up runs are excluded.

### mem_snapshot (light, PASS)

Command: `cat /proc/meminfo`

| Condition | Runs | CPU total (ms) | CPU self (ms) | Induced CPU (ms) | Wall (ms) | Memory (KB) | Output (KB) | Tasks | Failures |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| idle | 12 | 1 / 1 / 1 | 1 / 1 / 1 | 0 / 0 / 0 | 1 / 2 / 2 | 256 / 512 / 512 | 1.5 / 1.5 / 1.5 | 1 | rc!=0: 0, timeout: 0, capped: 0, leftover: 0 |
| typical | 12 | 1 / 1 / 1 | 1 / 1 / 1 | 0 / 0 / 0 | 2 / 3 / 3 | 256 / 512 / 512 | 1.5 / 1.5 / 1.5 | 1 | rc!=0: 0, timeout: 0, capped: 0, leftover: 0 |
| stressed | 12 | 1 / 1 / 1 | 1 / 1 / 1 | 0 / 0 / 0 | 2 / 4 / 4 | 256 / 512 / 512 | 1.5 / 1.5 / 1.5 | 1 | rc!=0: 0, timeout: 0, capped: 0, leftover: 0 |

Notes: fewer than 20 runs in a condition (min 12); p95 is approximate.

### proc_scan (light, PASS)

Command: `ls -l /proc/self/fd /proc/net`

| Condition | Runs | CPU total (ms) | CPU self (ms) | Induced CPU (ms) | Wall (ms) | Memory (KB) | Output (KB) | Tasks | Failures |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| idle | 12 | 2 / 3 / 3 | 2 / 3 / 3 | 0 / 0 / 0 | 3 / 4 / 4 | 768 / 860 / 860 | 0.4 / 0.5 / 0.5 | 2 | rc!=0: 0, timeout: 0, capped: 0, leftover: 0 |
| typical | 12 | 2 / 3 / 3 | 2 / 3 / 3 | 0 / 0 / 0 | 3 / 5 / 5 | 512 / 768 / 768 | 0.4 / 0.5 / 0.5 | 2 | rc!=0: 0, timeout: 0, capped: 0, leftover: 0 |
| stressed | 12 | 2 / 3 / 3 | 2 / 3 / 3 | 0 / 0 / 0 | 5 / 5 / 5 | 768 / 768 / 768 | 0.4 / 0.5 / 0.5 | 2 | rc!=0: 0, timeout: 0, capped: 0, leftover: 0 |

Notes: fewer than 20 runs in a condition (min 12); p95 is approximate.

### compress_work (standard, REVIEW)

Command: `head -c 8000000 /dev/urandom | gzip -1 > /dev/null`

| Condition | Runs | CPU total (ms) | CPU self (ms) | Induced CPU (ms) | Wall (ms) | Memory (KB) | Output (KB) | Tasks | Failures |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| idle | 12 | 283 / 296 / 296 | 283 / 296 / 296 | 0 / 0 / 0 | 256 / 270 / 270 | 1692 / 1752 / 1752 | 0.0 / 0.0 / 0.0 | 3 | rc!=0: 0, timeout: 0, capped: 0, leftover: 0 |
| typical | 12 | 276 / 302 / 302 | 276 / 302 / 302 | 0 / 0 / 0 | 278 / 305 / 305 | 1500 / 1732 / 1732 | 0.0 / 0.0 / 0.0 | 3 | rc!=0: 0, timeout: 0, capped: 0, leftover: 0 |
| stressed | 12 | 274 / 287 / 287 | 274 / 287 / 287 | 0 / 0 / 0 | 279 / 325 / 325 | 1628 / 1740 / 1740 | 0.0 / 0.0 / 0.0 | 3 | rc!=0: 0, timeout: 0, capped: 0, leftover: 0 |

Notes: declared class 'light' but measured 'standard'; fewer than 20 runs in a condition (min 12); p95 is approximate.

### big_buffer (heavy, REVIEW)

Command: `dd if=/dev/zero of=/dev/null bs=40M count=2`

| Condition | Runs | CPU total (ms) | CPU self (ms) | Induced CPU (ms) | Wall (ms) | Memory (KB) | Output (KB) | Tasks | Failures |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| idle | 12 | 24 / 26 / 26 | 24 / 26 / 26 | 0 / 0 / 0 | 25 / 27 / 27 | 41472 / 41472 / 41472 | 0.1 / 0.1 / 0.1 | 1 | rc!=0: 0, timeout: 0, capped: 0, leftover: 0 |
| typical | 12 | 24 / 27 / 27 | 24 / 27 / 27 | 0 / 0 / 0 | 25 / 28 / 28 | 41472 / 41472 / 41472 | 0.1 / 0.1 / 0.1 | 1 | rc!=0: 0, timeout: 0, capped: 0, leftover: 0 |
| stressed | 12 | 20 / 21 / 21 | 20 / 21 / 21 | 0 / 0 / 0 | 23 / 35 / 35 | 41472 / 41472 / 41472 | 0.1 / 0.1 / 0.1 | 1 | rc!=0: 0, timeout: 0, capped: 0, leftover: 0 |

Notes: declared class 'standard' but measured 'heavy'; fewer than 20 runs in a condition (min 12); p95 is approximate.

### fork_burst (light, PASS)

Command: `for i in 1 2 3 4 5 6 7 8 9 10; do sleep 0.3 & done; wait`

| Condition | Runs | CPU total (ms) | CPU self (ms) | Induced CPU (ms) | Wall (ms) | Memory (KB) | Output (KB) | Tasks | Failures |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| idle | 12 | 14 / 15 / 15 | 14 / 15 / 15 | 0 / 0 / 0 | 307 / 309 / 309 | 2952 / 3124 / 3124 | 0.0 / 0.0 / 0.0 | 11 | rc!=0: 0, timeout: 0, capped: 0, leftover: 0 |
| typical | 12 | 12 / 13 / 13 | 12 / 13 / 13 | 0 / 0 / 0 | 309 / 310 / 310 | 2896 / 3152 / 3152 | 0.0 / 0.0 / 0.0 | 11 | rc!=0: 0, timeout: 0, capped: 0, leftover: 0 |
| stressed | 12 | 12 / 13 / 13 | 12 / 13 / 13 | 0 / 0 / 0 | 311 / 316 / 316 | 3084 / 3104 / 3104 | 0.0 / 0.0 / 0.0 | 11 | rc!=0: 0, timeout: 0, capped: 0, leftover: 0 |

Notes: fewer than 20 runs in a condition (min 12); p95 is approximate.

### exits_nonzero (light, REVIEW)

Command: `ls /does/not/exist`

| Condition | Runs | CPU total (ms) | CPU self (ms) | Induced CPU (ms) | Wall (ms) | Memory (KB) | Output (KB) | Tasks | Failures |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| idle | 12 | 1 / 2 / 2 | 1 / 2 / 2 | 0 / 0 / 0 | 2 / 3 / 3 | 256 / 512 / 512 | 0.1 / 0.1 / 0.1 | 1 | rc!=0: 12, timeout: 0, capped: 0, leftover: 0 |
| typical | 12 | 1 / 1 / 1 | 1 / 1 / 1 | 0 / 0 / 0 | 2 / 2 / 2 | 256 / 512 / 512 | 0.1 / 0.1 / 0.1 | 1 | rc!=0: 12, timeout: 0, capped: 0, leftover: 0 |
| stressed | 12 | 1 / 1 / 1 | 1 / 1 / 1 | 0 / 0 / 0 | 2 / 2 / 2 | 512 / 512 / 512 | 0.1 / 0.1 / 0.1 | 1 | rc!=0: 12, timeout: 0, capped: 0, leftover: 0 |

Notes: non-zero exit in 12/12 runs (idle); non-zero exit in 12/12 runs (typical); non-zero exit in 12/12 runs (stressed); fewer than 20 runs in a condition (min 12); p95 is approximate.

### leaves_daemon (light, REVIEW)

Command: `sleep 30 > /dev/null 2>&1 & echo started`

| Condition | Runs | CPU total (ms) | CPU self (ms) | Induced CPU (ms) | Wall (ms) | Memory (KB) | Output (KB) | Tasks | Failures |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| idle | 12 | 1 / 2 / 2 | 1 / 2 / 2 | 0 / 0 / 0 | 1 / 2 / 2 | 768 / 768 / 768 | 0.0 / 0.0 / 0.0 | 2 | rc!=0: 0, timeout: 0, capped: 0, leftover: 12 |
| typical | 12 | 2 / 3 / 3 | 2 / 3 / 3 | 0 / 0 / 0 | 1 / 3 / 3 | 716 / 768 / 768 | 0.0 / 0.0 / 0.0 | 2 | rc!=0: 0, timeout: 0, capped: 0, leftover: 12 |
| stressed | 12 | 1 / 2 / 2 | 1 / 2 / 2 | 0 / 0 / 0 | 1 / 5 / 5 | 768 / 768 / 768 | 0.0 / 0.0 / 0.0 | 2 | rc!=0: 0, timeout: 0, capped: 0, leftover: 12 |

Notes: left processes behind in 12 run(s) (idle); left processes behind in 12 run(s) (typical); left processes behind in 12 run(s) (stressed); fewer than 20 runs in a condition (min 12); p95 is approximate.

### cpu_loop (unclassified, FAIL)

Command: `while :; do :; done`

| Condition | Runs | CPU total (ms) | CPU self (ms) | Induced CPU (ms) | Wall (ms) | Memory (KB) | Output (KB) | Tasks | Failures |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| idle | 3 | 3007 / 3008 / 3008 | 3007 / 3008 / 3008 | 0 / 0 / 0 | 3007 / 3009 / 3009 | 512 / 512 / 512 | 0.0 / 0.0 / 0.0 | 1 | rc!=0: 3, timeout: 3, capped: 0, leftover: 0 |
| typical | 3 | 2989 / 2998 / 2998 | 2989 / 2998 / 2998 | 0 / 0 / 0 | 3006 / 3010 / 3010 | 256 / 256 / 256 | 0.0 / 0.0 / 0.0 | 1 | rc!=0: 3, timeout: 3, capped: 0, leftover: 0 |
| stressed | 3 | 2836 / 2859 / 2859 | 2836 / 2859 / 2859 | 0 / 0 / 0 | 3004 / 3008 / 3008 | 256 / 512 / 512 | 0.0 / 0.0 / 0.0 | 1 | rc!=0: 3, timeout: 3, capped: 0, leftover: 0 |

Notes: CPU budget 4520 ms exceeds heavy limit 2000 ms; declared class 'light' but measured 'unclassified'; 3 timeout(s) in idle; non-zero exit in 3/3 runs (idle); 3 timeout(s) in typical; non-zero exit in 3/3 runs (typical); 3 timeout(s) in stressed; non-zero exit in 3/3 runs (stressed); fewer than 20 runs in a condition (min 3); p95 is approximate.

### endless_output (unclassified, FAIL)

Command: `yes`

| Condition | Runs | CPU total (ms) | CPU self (ms) | Induced CPU (ms) | Wall (ms) | Memory (KB) | Output (KB) | Tasks | Failures |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| idle | 3 | 1 / 2 / 2 | 1 / 2 / 2 | 0 / 0 / 0 | 2 / 2 / 2 | 656 / 704 / 704 | 1080.0 / 1096.0 / 1096.0 | 1 | rc!=0: 3, timeout: 0, capped: 3, leftover: 0 |
| typical | 3 | 1 / 1 / 1 | 1 / 1 / 1 | 0 / 0 / 0 | 1 / 2 / 2 | 512 / 712 / 712 | 1096.0 / 1096.0 / 1096.0 | 1 | rc!=0: 3, timeout: 0, capped: 3, leftover: 0 |
| stressed | 3 | 1 / 1 / 1 | 1 / 1 / 1 | 0 / 0 / 0 | 1 / 1 / 1 | 512 / 512 / 512 | 1096.0 / 1104.0 / 1104.0 | 1 | rc!=0: 3, timeout: 0, capped: 3, leftover: 0 |

Notes: output budget 1656 KB exceeds heavy limit 64 KB; declared class 'light' but measured 'unclassified'; output hard-capped 3 time(s) in idle; non-zero exit in 3/3 runs (idle); output hard-capped 3 time(s) in typical; non-zero exit in 3/3 runs (typical); output hard-capped 3 time(s) in stressed; non-zero exit in 3/3 runs (stressed); fewer than 20 runs in a condition (min 3); p95 is approximate.

## Class limits used

| Class | CPU (ms) | Wall (ms) | Memory (KB) | Output (KB) |
| --- | --- | --- | --- | --- |
| light | 200 | 2000 | 8192 | 16 |
| standard | 1000 | 10000 | 32768 | 64 |
| heavy | 2000 | 10000 | 65536 | 64 |

Verdicts: PASS = fits a class with no issues. REVIEW = fits a class but has exit errors, leftover processes, mostly induced CPU, unstable timing or a class different from the one declared. FAIL = exceeds every class, timed out, or hit the output hard cap.

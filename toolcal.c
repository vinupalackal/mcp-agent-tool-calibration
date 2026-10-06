/*
 * toolcal - tool resource calibration framework for the CPE Agent
 *
 * Reads a JSON tool catalog, runs every tool N times inside its own cgroup,
 * measures CPU, memory, task count, IO, wall time, output size and the CPU
 * the tool induces in other daemons (CCSP components, rbus), then builds a
 * threshold report (Markdown + JSON + CSV) with a budget and class per tool.
 *
 * Subcommands
 *   toolcal list   -c tools.json
 *   toolcal run    -c tools.json -o <dir> [-l condition] [-n runs] [-t tool]
 *   toolcal report -c tools.json -o <prefix> [-b basis] runs.csv [runs.csv ...]
 *   toolcal hog    [--cpu N] [--mem-mb M] [--seconds S]
 *
 * Measurement backends (auto-detected, or forced with settings.cgroup_mode)
 *   v2   : cpu.stat, memory.peak (or polled memory.current), pids.peak
 *          (or polled pids.current), io.stat
 *   v1   : cpuacct.usage, memory.max_usage_in_bytes, polled pids.current,
 *          blkio.throttle.io_service_bytes
 *   none : wait4() rusage only (ru_maxrss = largest single process)
 *
 * Build: see Makefile. Needs cJSON (system -lcjson or vendored cJSON.c).
 * Run as root for cgroup backends.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef CJSON_SYSTEM
#include <cjson/cJSON.h>
#else
#include "cJSON.h"
#endif

#define TOOLCAL_VERSION "1.0.0"
#define MAX_TOOLS 256
#define MAX_ARGS 64
#define MAX_NAMES 32
#define MAX_CLASSES 8
#define PATH_LEN 512

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[32];
    double cpu_ms, wall_ms, mem_kb, out_kb;
} class_t;

typedef struct {
    char name[64];
    char plane[32];
    char declared_class[32];
    int shell;              /* 1 = run via /bin/sh -c */
    char *command;          /* display / shell string */
    char *argv[MAX_ARGS + 1];
    int argc;
    int runs;               /* 0 = use settings */
    long timeout_ms;        /* 0 = use settings */
    char *induced[MAX_NAMES];
    int n_induced;          /* -1 = use settings list */
} tool_t;

typedef struct {
    int runs;
    int warmup;
    long timeout_ms;
    long grace_ms;
    long poll_ms;
    long cooldown_ms;
    long save_output_kb;
    long hard_output_kb;
    double budget_factor;
    char cgroup_mode[8];      /* auto | v2 | v1 | none */
    char cgroup_v2_root[PATH_LEN];
    char cgroup_v1_root[PATH_LEN];
    char group[64];
    char *induced[MAX_NAMES];
    int n_induced;
    double min_cpu_ms, min_wall_ms, min_mem_kb, min_out_kb;
} settings_t;

typedef struct {
    settings_t s;
    class_t classes[MAX_CLASSES];
    int n_classes;
    tool_t tools[MAX_TOOLS];
    int n_tools;
} config_t;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "toolcal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(2);
}

static void warnx(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "toolcal: warning: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static char *xstrdup(const char *s)
{
    char *p = strdup(s ? s : "");
    if (!p) die("out of memory");
    return p;
}

/* Build a path; stop with a clear error if it would be truncated. */
static void pj(char *dst, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= n) die("path too long: %.80s...", dst);
}

/* Bounded copy that always NUL-terminates (truncation is intended). */
static void safe_copy(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    if (!n) return;
    if (src)
        for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* Replace characters that would break CSV or paths. */
static void sanitize_name(char *s)
{
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '-' || *s == '.'))
            *s = '_';
}

static char *read_whole_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) die("out of memory");
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

static double jnum(const cJSON *o, const char *k, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static const char *jstr(const cJSON *o, const char *k, const char *def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : def;
}

static int jbool(const cJSON *o, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return def;
}

static int load_names(const cJSON *arr, char **out, int max)
{
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsString(it) && n < max) out[n++] = xstrdup(it->valuestring);
    }
    return n;
}

/* Whitespace tokenizer that honours simple single/double quotes. */
static int tokenize(const char *cmd, char **argv, int max)
{
    int argc = 0;
    const char *p = cmd;
    while (*p && argc < max) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char buf[1024];
        size_t bl = 0;
        char q = 0;
        while (*p && (q || !isspace((unsigned char)*p))) {
            if (!q && (*p == '\'' || *p == '"')) { q = *p++; continue; }
            if (q && *p == q) { q = 0; p++; continue; }
            if (bl < sizeof(buf) - 1) buf[bl++] = *p;
            p++;
        }
        buf[bl] = 0;
        argv[argc++] = xstrdup(buf);
    }
    argv[argc] = NULL;
    return argc;
}

static void default_classes(config_t *c)
{
    const class_t d[] = {
        {"light", 200, 2000, 8192, 16},
        {"standard", 1000, 10000, 32768, 64},
        {"heavy", 2000, 10000, 65536, 64},
    };
    c->n_classes = 3;
    memcpy(c->classes, d, sizeof(d));
}

static void load_config(const char *path, config_t *c)
{
    memset(c, 0, sizeof(*c));
    char *txt = read_whole_file(path);
    if (!txt) die("cannot read %s: %s", path, strerror(errno));
    cJSON *root = cJSON_Parse(txt);
    if (!root) die("%s: invalid JSON near: %.40s", path, cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?");

    const cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "settings");
    settings_t *st = &c->s;
    st->runs = (int)jnum(s, "runs", 30);
    st->warmup = (int)jnum(s, "warmup", 1);
    st->timeout_ms = (long)jnum(s, "timeout_ms", 10000);
    st->grace_ms = (long)jnum(s, "grace_ms", 2000);
    st->poll_ms = (long)jnum(s, "poll_ms", 10);
    st->cooldown_ms = (long)jnum(s, "cooldown_ms", 200);
    st->save_output_kb = (long)jnum(s, "save_output_kb", 64);
    st->hard_output_kb = (long)jnum(s, "hard_output_kb", 4096);
    st->budget_factor = jnum(s, "budget_factor", 1.5);
    safe_copy(st->cgroup_mode, sizeof(st->cgroup_mode), jstr(s, "cgroup_mode", "auto"));
    safe_copy(st->cgroup_v2_root, sizeof(st->cgroup_v2_root), jstr(s, "cgroup_v2_root", "/sys/fs/cgroup"));
    safe_copy(st->cgroup_v1_root, sizeof(st->cgroup_v1_root), jstr(s, "cgroup_v1_root", "/sys/fs/cgroup"));
    safe_copy(st->group, sizeof(st->group), jstr(s, "group", "toolcal"));
    st->min_cpu_ms = jnum(s, "min_cpu_ms", 10);
    st->min_wall_ms = jnum(s, "min_wall_ms", 100);
    st->min_mem_kb = jnum(s, "min_mem_kb", 1024);
    st->min_out_kb = jnum(s, "min_out_kb", 1);
    const cJSON *ind = cJSON_GetObjectItemCaseSensitive(s, "induced_procs");
    if (cJSON_IsArray(ind)) {
        st->n_induced = load_names(ind, st->induced, MAX_NAMES);
    } else {
        const char *d[] = {"CcspPandMSsp", "CcspWifiSsp", "CcspCrSsp", "PsmSsp", "rtrouted", "CcspCMAgentSsp"};
        for (size_t i = 0; i < sizeof(d) / sizeof(d[0]); i++) st->induced[st->n_induced++] = xstrdup(d[i]);
    }
    if (st->runs < 1) st->runs = 1;
    if (st->poll_ms < 1) st->poll_ms = 1;

    const cJSON *cls = cJSON_GetObjectItemCaseSensitive(root, "classes");
    if (cJSON_IsArray(cls) && cJSON_GetArraySize(cls) > 0) {
        const cJSON *it;
        cJSON_ArrayForEach(it, cls) {
            if (c->n_classes >= MAX_CLASSES) break;
            class_t *k = &c->classes[c->n_classes++];
            safe_copy(k->name, sizeof(k->name), jstr(it, "name", "class"));
            k->cpu_ms = jnum(it, "cpu_ms", 0);
            k->wall_ms = jnum(it, "wall_ms", 0);
            k->mem_kb = jnum(it, "mem_kb", 0);
            k->out_kb = jnum(it, "out_kb", 0);
        }
    } else {
        default_classes(c);
    }

    const cJSON *tools = cJSON_GetObjectItemCaseSensitive(root, "tools");
    if (!cJSON_IsArray(tools)) die("%s: \"tools\" array missing", path);
    const cJSON *t;
    cJSON_ArrayForEach(t, tools) {
        if (c->n_tools >= MAX_TOOLS) { warnx("more than %d tools; extra ignored", MAX_TOOLS); break; }
        tool_t *tl = &c->tools[c->n_tools];
        memset(tl, 0, sizeof(*tl));
        const char *nm = jstr(t, "name", NULL);
        if (!nm) { warnx("tool without \"name\" skipped"); continue; }
        safe_copy(tl->name, sizeof(tl->name), nm);
        sanitize_name(tl->name);
        safe_copy(tl->plane, sizeof(tl->plane), jstr(t, "plane", "-"));
        safe_copy(tl->declared_class, sizeof(tl->declared_class), jstr(t, "class", ""));
        tl->shell = jbool(t, "shell", 0);
        tl->runs = (int)jnum(t, "runs", 0);
        tl->timeout_ms = (long)jnum(t, "timeout_ms", 0);
        const cJSON *ti = cJSON_GetObjectItemCaseSensitive(t, "induced_procs");
        tl->n_induced = cJSON_IsArray(ti) ? load_names(ti, tl->induced, MAX_NAMES) : -1;

        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(t, "command");
        if (cJSON_IsArray(cmd)) {
            const cJSON *a;
            char disp[2048] = "";
            cJSON_ArrayForEach(a, cmd) {
                if (!cJSON_IsString(a) || tl->argc >= MAX_ARGS) continue;
                tl->argv[tl->argc++] = xstrdup(a->valuestring);
                if (disp[0]) strncat(disp, " ", sizeof(disp) - strlen(disp) - 1);
                strncat(disp, a->valuestring, sizeof(disp) - strlen(disp) - 1);
            }
            tl->argv[tl->argc] = NULL;
            tl->command = xstrdup(disp);
            tl->shell = 0;
        } else if (cJSON_IsString(cmd)) {
            tl->command = xstrdup(cmd->valuestring);
            if (!tl->shell) tl->argc = tokenize(cmd->valuestring, tl->argv, MAX_ARGS);
        }
        if (!tl->command || (!tl->shell && tl->argc == 0)) {
            warnx("tool %s has no usable \"command\"; skipped", tl->name);
            continue;
        }
        c->n_tools++;
    }
    cJSON_Delete(root);
    free(txt);
}

/* ------------------------------------------------------------------ */
/* Small file helpers                                                  */
/* ------------------------------------------------------------------ */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int write_str(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, s, strlen(s));
    int e = errno;
    close(fd);
    errno = e;
    return n == (ssize_t)strlen(s) ? 0 : -1;
}

static int read_small(const char *path, char *buf, size_t len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return (int)n;
}

static long long read_ll(const char *path)
{
    char b[128];
    if (read_small(path, b, sizeof(b)) <= 0) return -1;
    if (!strncmp(b, "max", 3)) return -1;
    return strtoll(b, NULL, 10);
}

static long long read_keyed(const char *path, const char *key)
{
    char b[4096];
    if (read_small(path, b, sizeof(b)) <= 0) return -1;
    size_t kl = strlen(key);
    for (char *line = strtok(b, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, key, kl) && line[kl] == ' ') return strtoll(line + kl + 1, NULL, 10);
    }
    return -1;
}

static int path_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static void mkdir_p(const char *path)
{
    char tmp[PATH_LEN];
    safe_copy(tmp, sizeof(tmp), path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

/* ------------------------------------------------------------------ */
/* cgroup backends                                                     */
/* ------------------------------------------------------------------ */

typedef enum { CG_NONE = 0, CG_V1, CG_V2 } cgmode_t;

typedef struct {
    cgmode_t mode;
    char v2_root[PATH_LEN], v2_base[PATH_LEN];
    char v1_cpu[PATH_LEN], v1_mem[PATH_LEN], v1_pids[PATH_LEN], v1_blkio[PATH_LEN];
} cgctx_t;

typedef struct {
    char v2[PATH_LEN];
    char cpu[PATH_LEN], mem[PATH_LEN], pids[PATH_LEN], blkio[PATH_LEN];
} cgrun_t;

static const char *cg_mode_name(cgmode_t m)
{
    return m == CG_V2 ? "cgroup-v2" : m == CG_V1 ? "cgroup-v1" : "rusage";
}

static int v2_usable(const char *root)
{
    char b[512], p[PATH_LEN];
    pj(p, sizeof(p), "%s/cgroup.controllers", root);
    if (read_small(p, b, sizeof(b)) < 0) return 0;
    return strstr(b, "memory") != NULL && strstr(b, "cpu") != NULL;
}

static int v1_find(const char *root, const char *const *cands, char *out)
{
    for (int i = 0; cands[i]; i++) {
        char p[PATH_LEN];
        pj(p, sizeof(p), "%s/%s", root, cands[i]);
        if (path_exists(p)) { safe_copy(out, PATH_LEN, p); return 1; }
    }
    out[0] = 0;
    return 0;
}

static int cg_try_v2(cgctx_t *cg, const settings_t *s)
{
    if (!v2_usable(s->cgroup_v2_root)) return 0;
    safe_copy(cg->v2_root, sizeof(cg->v2_root), s->cgroup_v2_root);
    pj(cg->v2_base, sizeof(cg->v2_base), "%s/%s", s->cgroup_v2_root, s->group);
    mkdir(cg->v2_base, 0755);
    if (!path_exists(cg->v2_base)) return 0;
    const char *ctl[] = {"+cpu", "+memory", "+pids", "+io"};
    char p[PATH_LEN];
    for (int i = 0; i < 4; i++) {
        pj(p, sizeof(p), "%s/cgroup.subtree_control", cg->v2_root);
        write_str(p, ctl[i]);
        pj(p, sizeof(p), "%s/cgroup.subtree_control", cg->v2_base);
        write_str(p, ctl[i]);
    }
    cg->mode = CG_V2;
    return 1;
}

static int cg_try_v1(cgctx_t *cg, const settings_t *s)
{
    const char *cpu[] = {"cpuacct", "cpu,cpuacct", "cpuacct,cpu", NULL};
    const char *mem[] = {"memory", NULL};
    const char *pid[] = {"pids", NULL};
    const char *blk[] = {"blkio", NULL};
    char base[PATH_LEN];
    if (!v1_find(s->cgroup_v1_root, cpu, base)) return 0;
    pj(cg->v1_cpu, PATH_LEN, "%s/%s", base, s->group);
    if (!v1_find(s->cgroup_v1_root, mem, base)) return 0;
    pj(cg->v1_mem, PATH_LEN, "%s/%s", base, s->group);
    if (v1_find(s->cgroup_v1_root, pid, base)) pj(cg->v1_pids, PATH_LEN, "%s/%s", base, s->group);
    if (v1_find(s->cgroup_v1_root, blk, base)) pj(cg->v1_blkio, PATH_LEN, "%s/%s", base, s->group);
    mkdir(cg->v1_cpu, 0755);
    mkdir(cg->v1_mem, 0755);
    if (cg->v1_pids[0]) mkdir(cg->v1_pids, 0755);
    if (cg->v1_blkio[0]) mkdir(cg->v1_blkio, 0755);
    if (!path_exists(cg->v1_cpu) || !path_exists(cg->v1_mem)) return 0;
    if (cg->v1_pids[0] && !path_exists(cg->v1_pids)) cg->v1_pids[0] = 0;
    if (cg->v1_blkio[0] && !path_exists(cg->v1_blkio)) cg->v1_blkio[0] = 0;
    cg->mode = CG_V1;
    return 1;
}

static void cg_init(cgctx_t *cg, const settings_t *s)
{
    memset(cg, 0, sizeof(*cg));
    const char *m = s->cgroup_mode;
    if (geteuid() != 0 && strcmp(m, "none")) {
        warnx("not running as root; using rusage only (no cgroups)");
        return;
    }
    if (!strcmp(m, "none")) return;
    if (!strcmp(m, "v2")) { if (!cg_try_v2(cg, s)) die("cgroup v2 requested but not usable at %s", s->cgroup_v2_root); return; }
    if (!strcmp(m, "v1")) { if (!cg_try_v1(cg, s)) die("cgroup v1 requested but not usable at %s", s->cgroup_v1_root); return; }
    if (cg_try_v2(cg, s)) return;
    if (cg_try_v1(cg, s)) return;
    warnx("no usable cgroup hierarchy; using rusage only");
}

static void cg_run_paths(const cgctx_t *cg, cgrun_t *r, const char *leaf)
{
    memset(r, 0, sizeof(*r));
    if (cg->mode == CG_V2) {
        pj(r->v2, PATH_LEN, "%s/%s", cg->v2_base, leaf);
    } else if (cg->mode == CG_V1) {
        pj(r->cpu, PATH_LEN, "%s/%s", cg->v1_cpu, leaf);
        pj(r->mem, PATH_LEN, "%s/%s", cg->v1_mem, leaf);
        if (cg->v1_pids[0]) pj(r->pids, PATH_LEN, "%s/%s", cg->v1_pids, leaf);
        if (cg->v1_blkio[0]) pj(r->blkio, PATH_LEN, "%s/%s", cg->v1_blkio, leaf);
    }
}

static int cg_create(const cgctx_t *cg, const cgrun_t *r)
{
    if (cg->mode == CG_V2) return mkdir(r->v2, 0755);
    if (cg->mode == CG_V1) {
        if (mkdir(r->cpu, 0755) || mkdir(r->mem, 0755)) return -1;
        if (r->pids[0]) mkdir(r->pids, 0755);
        if (r->blkio[0]) mkdir(r->blkio, 0755);
    }
    return 0;
}

/* Called in the child before exec: move ourselves into the run cgroup. */
static int cg_attach_self(const cgctx_t *cg, const cgrun_t *r)
{
    char p[PATH_LEN + 32], pid[32];
    pj(pid, sizeof(pid), "%d", (int)getpid());
    int rc = 0;
    if (cg->mode == CG_V2) {
        pj(p, sizeof(p), "%s/cgroup.procs", r->v2);
        rc |= write_str(p, pid);
    } else if (cg->mode == CG_V1) {
        const char *dirs[] = {r->cpu, r->mem, r->pids, r->blkio};
        for (int i = 0; i < 4; i++) {
            if (!dirs[i][0]) continue;
            pj(p, sizeof(p), "%s/cgroup.procs", dirs[i]);
            int e = write_str(p, pid);
            if (i < 2) rc |= e;
        }
    }
    return rc;
}

static void cg_sample(const cgctx_t *cg, const cgrun_t *r, long long *mem_b, long long *pids)
{
    char p[PATH_LEN + 32];
    *mem_b = *pids = -1;
    if (cg->mode == CG_V2) {
        pj(p, sizeof(p), "%s/memory.current", r->v2);
        *mem_b = read_ll(p);
        pj(p, sizeof(p), "%s/pids.current", r->v2);
        *pids = read_ll(p);
    } else if (cg->mode == CG_V1) {
        pj(p, sizeof(p), "%s/memory.usage_in_bytes", r->mem);
        *mem_b = read_ll(p);
        if (r->pids[0]) {
            pj(p, sizeof(p), "%s/pids.current", r->pids);
            *pids = read_ll(p);
        }
    }
}

typedef struct {
    long long cpu_us, mem_peak_b, pids_peak, rbytes, wbytes;
} cgstat_t;

static void parse_io_v2(const char *path, long long *r, long long *w)
{
    char b[4096];
    *r = *w = -1;
    if (read_small(path, b, sizeof(b)) < 0) return;
    *r = *w = 0;
    for (char *tok = strtok(b, " \n"); tok; tok = strtok(NULL, " \n")) {
        if (!strncmp(tok, "rbytes=", 7)) *r += strtoll(tok + 7, NULL, 10);
        else if (!strncmp(tok, "wbytes=", 7)) *w += strtoll(tok + 7, NULL, 10);
    }
}

static void parse_io_v1(const char *path, long long *r, long long *w)
{
    char b[8192];
    *r = *w = -1;
    if (read_small(path, b, sizeof(b)) < 0) return;
    *r = *w = 0;
    for (char *line = strtok(b, "\n"); line; line = strtok(NULL, "\n")) {
        char dev[64], op[32];
        long long v;
        if (sscanf(line, "%63s %31s %lld", dev, op, &v) == 3) {
            if (!strcmp(op, "Read")) *r += v;
            else if (!strcmp(op, "Write")) *w += v;
        }
    }
}

static void cg_collect(const cgctx_t *cg, const cgrun_t *r, cgstat_t *st)
{
    char p[PATH_LEN + 48];
    st->cpu_us = st->mem_peak_b = st->pids_peak = st->rbytes = st->wbytes = -1;
    if (cg->mode == CG_V2) {
        pj(p, sizeof(p), "%s/cpu.stat", r->v2);
        st->cpu_us = read_keyed(p, "usage_usec");
        pj(p, sizeof(p), "%s/memory.peak", r->v2);
        st->mem_peak_b = read_ll(p);
        pj(p, sizeof(p), "%s/pids.peak", r->v2);
        st->pids_peak = read_ll(p);
        pj(p, sizeof(p), "%s/io.stat", r->v2);
        parse_io_v2(p, &st->rbytes, &st->wbytes);
    } else if (cg->mode == CG_V1) {
        pj(p, sizeof(p), "%s/cpuacct.usage", r->cpu);
        long long ns = read_ll(p);
        st->cpu_us = ns >= 0 ? ns / 1000 : -1;
        pj(p, sizeof(p), "%s/memory.max_usage_in_bytes", r->mem);
        st->mem_peak_b = read_ll(p);
        if (r->blkio[0]) {
            pj(p, sizeof(p), "%s/blkio.throttle.io_service_bytes", r->blkio);
            parse_io_v1(p, &st->rbytes, &st->wbytes);
        }
    }
}

/* Kill anything still in the run cgroup (daemonised children). Returns count. */
static int cg_kill_leftovers(const cgctx_t *cg, const cgrun_t *r)
{
    if (cg->mode == CG_NONE) return 0;
    char p[PATH_LEN + 32];
    pj(p, sizeof(p), "%s/cgroup.procs", cg->mode == CG_V2 ? r->v2 : r->mem);
    int first_count = -1;
    for (int attempt = 0; attempt < 50; attempt++) {
        char b[8192];
        if (read_small(p, b, sizeof(b)) < 0) return first_count < 0 ? 0 : first_count;
        int n = 0;
        for (char *tok = strtok(b, "\n"); tok; tok = strtok(NULL, "\n")) {
            pid_t pid = (pid_t)atoi(tok);
            if (pid > 0) { kill(pid, SIGKILL); n++; }
        }
        if (first_count < 0) first_count = n;
        if (n == 0) break;
        usleep(10000);
    }
    return first_count < 0 ? 0 : first_count;
}

static void cg_destroy(const cgctx_t *cg, const cgrun_t *r)
{
    const char *dirs[] = {r->v2, r->cpu, r->mem, r->pids, r->blkio};
    if (cg->mode == CG_NONE) return;
    for (int i = 0; i < 5; i++) {
        if (!dirs[i][0]) continue;
        for (int k = 0; k < 20 && rmdir(dirs[i]) != 0 && errno == EBUSY; k++) usleep(10000);
    }
}

static void cg_cleanup_base(const cgctx_t *cg)
{
    if (cg->mode == CG_V2) rmdir(cg->v2_base);
    if (cg->mode == CG_V1) {
        rmdir(cg->v1_cpu);
        rmdir(cg->v1_mem);
        if (cg->v1_pids[0]) rmdir(cg->v1_pids);
        if (cg->v1_blkio[0]) rmdir(cg->v1_blkio);
    }
}

/* ------------------------------------------------------------------ */
/* System-wide snapshots: induced CPU, total CPU, PSI                  */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned long long induced_ticks;
    unsigned long long sys_busy_ticks;
    long long psi_cpu_us, psi_mem_us;
} sysnap_t;

static unsigned long long ticks_for_names(char *const *names, int n)
{
    if (n <= 0) return 0;
    DIR *d = opendir("/proc");
    if (!d) return 0;
    unsigned long long sum = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char p[300], b[1024];
        pj(p, sizeof(p), "/proc/%s/stat", de->d_name);
        if (read_small(p, b, sizeof(b)) <= 0) continue;
        char *lp = strchr(b, '('), *rp = strrchr(b, ')');
        if (!lp || !rp || rp < lp) continue;
        char comm[64];
        size_t cl = (size_t)(rp - lp - 1);
        if (cl >= sizeof(comm)) cl = sizeof(comm) - 1;
        memcpy(comm, lp + 1, cl);
        comm[cl] = 0;
        int match = 0;
        for (int i = 0; i < n && !match; i++) match = !strncmp(comm, names[i], 15);
        if (!match) continue;
        char state;
        int ppid, pgrp, sess, tty, tpgid;
        unsigned int flags;
        unsigned long minflt, cminflt, majflt, cmajflt;
        unsigned long long ut, stt;
        if (sscanf(rp + 2, "%c %d %d %d %d %d %u %lu %lu %lu %lu %llu %llu", &state, &ppid, &pgrp, &sess,
                   &tty, &tpgid, &flags, &minflt, &cminflt, &majflt, &cmajflt, &ut, &stt) == 13)
            sum += ut + stt;
    }
    closedir(d);
    return sum;
}

static unsigned long long sys_busy_ticks(void)
{
    char b[512];
    if (read_small("/proc/stat", b, sizeof(b)) <= 0) return 0;
    unsigned long long u = 0, ni = 0, sy = 0, id = 0, io = 0, irq = 0, sirq = 0, st = 0;
    sscanf(b, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &ni, &sy, &id, &io, &irq, &sirq, &st);
    return u + ni + sy + irq + sirq + st;
}

static long long psi_total(const char *path)
{
    char b[512];
    if (read_small(path, b, sizeof(b)) <= 0) return -1;
    char *t = strstr(b, "total=");
    return t ? strtoll(t + 6, NULL, 10) : -1;
}

static void snap(sysnap_t *s, char *const *names, int n)
{
    s->induced_ticks = ticks_for_names(names, n);
    s->sys_busy_ticks = sys_busy_ticks();
    s->psi_cpu_us = psi_total("/proc/pressure/cpu");
    s->psi_mem_us = psi_total("/proc/pressure/memory");
}

/* ------------------------------------------------------------------ */
/* One measured run                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int rc, sig, timed_out, capped, leftover;
    long long wall_ms, cpu_us, ru_cpu_us, mem_peak_b, pids_peak, rbytes, wbytes, out_bytes;
    long long induced_ms, other_ms, psi_cpu_us, psi_mem_us;
} result_t;

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void run_once(const config_t *c, const cgctx_t *cg, const tool_t *t, int run_idx, const char *save_path,
                     result_t *res)
{
    const settings_t *s = &c->s;
    memset(res, 0, sizeof(*res));
    res->rc = -1;
    res->mem_peak_b = res->pids_peak = res->rbytes = res->wbytes = -1;
    long timeout = t->timeout_ms > 0 ? t->timeout_ms : s->timeout_ms;
    char *const *names = t->n_induced >= 0 ? t->induced : s->induced;
    int n_names = t->n_induced >= 0 ? t->n_induced : s->n_induced;
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;

    char leaf[160];
    pj(leaf, sizeof(leaf), "%s.%d.%d", t->name, run_idx, (int)getpid());
    cgrun_t r;
    cg_run_paths(cg, &r, leaf);
    int have_cg = cg->mode != CG_NONE && cg_create(cg, &r) == 0;
    if (cg->mode != CG_NONE && !have_cg) warnx("%s: cannot create run cgroup (%s); rusage only for this run", t->name, strerror(errno));

    FILE *save = save_path ? fopen(save_path, "w") : NULL;
    long long save_left = s->save_output_kb * 1024;

    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) die("pipe: %s", strerror(errno));

    sysnap_t before, after;
    snap(&before, names, n_names);

    /* sync pipe: the child reports once it has joined its cgroup, so the
     * cgroup migration cost (slow on v1) is not counted as tool time. */
    int sfd[2];
    if (pipe2(sfd, O_CLOEXEC) != 0) die("pipe: %s", strerror(errno));
    pid_t pid = fork();
    if (pid < 0) die("fork: %s", strerror(errno));
    if (pid == 0) {
        setpgid(0, 0);
        close(sfd[0]);
        if (have_cg && cg_attach_self(cg, &r) != 0) _exit(126);
        if (write(sfd[1], "1", 1) != 1) _exit(126);
        close(sfd[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, 0); close(devnull); }
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        signal(SIGPIPE, SIG_DFL);
        if (t->shell) execl("/bin/sh", "sh", "-c", t->command, (char *)NULL);
        else execvp(t->argv[0], t->argv);
        dprintf(2, "toolcal: exec %s failed: %s\n", t->shell ? "/bin/sh" : t->argv[0], strerror(errno));
        _exit(127);
    }
    setpgid(pid, pid);
    close(sfd[1]);
    {
        char ok;
        struct pollfd sp = {sfd[0], POLLIN, 0};
        if (poll(&sp, 1, 5000) > 0) (void)!read(sfd[0], &ok, 1);
        close(sfd[0]);
    }
    long long t0 = now_ms();
    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL) | O_NONBLOCK);

    int pipe_open = 1, reaped = 0, status = 0;
    long long t_end = 0, term_at = 0, eof_at = 0, poll_mem = -1, poll_pids = -1;
    struct rusage ru;
    memset(&ru, 0, sizeof(ru));
    char buf[8192];

    for (;;) {
        if (pipe_open) {
            struct pollfd pf = {pfd[0], POLLIN, 0};
            poll(&pf, 1, (int)s->poll_ms);
            for (;;) {
                ssize_t n = read(pfd[0], buf, sizeof(buf));
                if (n > 0) {
                    res->out_bytes += n;
                    if (save && save_left > 0) {
                        size_t w = (size_t)(n < save_left ? n : save_left);
                        fwrite(buf, 1, w, save);
                        save_left -= (long long)w;
                    }
                    if (res->out_bytes > s->hard_output_kb * 1024 && !res->capped) {
                        res->capped = 1;
                        kill(-pid, SIGKILL);
                    }
                    continue;
                }
                if (n == 0) { pipe_open = 0; close(pfd[0]); }
                break;
            }
        } else if (!reaped) {
            /* Output closed: the child is usually about to exit, so check
             * often for a while to keep wall time accurate, then back off. */
            if (eof_at == 0) eof_at = now_ms();
            poll(NULL, 0, (now_ms() - eof_at < 100) ? 1 : (int)s->poll_ms);
        }

        if (have_cg) {
            long long m, p;
            cg_sample(cg, &r, &m, &p);
            if (m > poll_mem) poll_mem = m;
            if (p > poll_pids) poll_pids = p;
        }

        long long now = now_ms();
        if (!reaped) {
            pid_t w = wait4(pid, &status, WNOHANG, &ru);
            if (w == pid) { reaped = 1; t_end = now; }
        }
        if (!reaped) {
            if (!term_at && now - t0 > timeout) {
                res->timed_out = 1;
                term_at = now;
                kill(-pid, SIGTERM);
            } else if (term_at && now - term_at > s->grace_ms) {
                kill(-pid, SIGKILL);
            }
            if (g_stop) kill(-pid, SIGKILL);
        }
        if (reaped && (!pipe_open || now - t_end > 100)) break;
    }
    if (pipe_open) close(pfd[0]);
    if (save) fclose(save);

    res->wall_ms = t_end - t0;
    if (WIFEXITED(status)) res->rc = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) { res->sig = WTERMSIG(status); res->rc = 128 + res->sig; }
    res->ru_cpu_us = (long long)ru.ru_utime.tv_sec * 1000000 + ru.ru_utime.tv_usec +
                     (long long)ru.ru_stime.tv_sec * 1000000 + ru.ru_stime.tv_usec;

    /* Anything still alive in the tool's process group was left behind. */
    if (kill(-pid, 0) == 0) {
        if (!have_cg) res->leftover = 1;
        kill(-pid, SIGKILL);
    }

    if (have_cg) {
        int n = cg_kill_leftovers(cg, &r);
        if (n > res->leftover) res->leftover = n;
        cgstat_t cs;
        cg_collect(cg, &r, &cs);
        res->cpu_us = cs.cpu_us >= 0 ? cs.cpu_us : res->ru_cpu_us;
        res->mem_peak_b = cs.mem_peak_b >= 0 ? cs.mem_peak_b : poll_mem;
        if (poll_mem > res->mem_peak_b) res->mem_peak_b = poll_mem;
        res->pids_peak = cs.pids_peak >= 0 ? cs.pids_peak : poll_pids;
        res->rbytes = cs.rbytes;
        res->wbytes = cs.wbytes;
        cg_destroy(cg, &r);
    } else {
        res->cpu_us = res->ru_cpu_us;
        res->mem_peak_b = (long long)ru.ru_maxrss * 1024;
        res->rbytes = (long long)ru.ru_inblock * 512;
        res->wbytes = (long long)ru.ru_oublock * 512;
    }

    snap(&after, names, n_names);
    long long ind = (long long)(after.induced_ticks - before.induced_ticks);
    if (after.induced_ticks < before.induced_ticks) ind = 0;
    res->induced_ms = ind * 1000 / hz;
    long long sys_ms = (long long)(after.sys_busy_ticks - before.sys_busy_ticks) * 1000 / hz;
    res->other_ms = sys_ms - res->cpu_us / 1000 - res->induced_ms;
    if (res->other_ms < 0) res->other_ms = 0;
    res->psi_cpu_us = (before.psi_cpu_us >= 0 && after.psi_cpu_us >= 0) ? after.psi_cpu_us - before.psi_cpu_us : -1;
    res->psi_mem_us = (before.psi_mem_us >= 0 && after.psi_mem_us >= 0) ? after.psi_mem_us - before.psi_mem_us : -1;
}

/* ------------------------------------------------------------------ */
/* Subcommand: list                                                    */
/* ------------------------------------------------------------------ */

static int cmd_list(const config_t *c)
{
    printf("%-24s %-12s %-10s %-6s %s\n", "TOOL", "PLANE", "CLASS", "RUNS", "COMMAND");
    for (int i = 0; i < c->n_tools; i++) {
        const tool_t *t = &c->tools[i];
        printf("%-24s %-12s %-10s %-6d %s%s\n", t->name, t->plane, t->declared_class[0] ? t->declared_class : "-",
               t->runs > 0 ? t->runs : c->s.runs, t->shell ? "[sh] " : "", t->command);
    }
    printf("\n%d tool(s). Classes:", c->n_tools);
    for (int i = 0; i < c->n_classes; i++)
        printf(" %s(cpu %.0f ms, wall %.0f ms, mem %.0f KB, out %.0f KB)%s", c->classes[i].name, c->classes[i].cpu_ms,
               c->classes[i].wall_ms, c->classes[i].mem_kb, c->classes[i].out_kb, i + 1 < c->n_classes ? ";" : "\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Subcommand: run                                                     */
/* ------------------------------------------------------------------ */

#define CSV_HEADER                                                                                               \
    "tool,plane,condition,run,warmup,rc,signal,timed_out,output_capped,wall_ms,cpu_ms,rusage_cpu_ms,mem_kb,"   \
    "pids,read_kb,write_kb,out_bytes,induced_cpu_ms,other_cpu_ms,psi_cpu_us,psi_mem_us,leftover_procs,backend\n"

static int cmd_run(const config_t *c, const char *outdir, const char *cond, int runs_override, const char *only)
{
    cgctx_t cg;
    cg_init(&cg, &c->s);
    mkdir_p(outdir);
    char p[PATH_LEN + 64];
    pj(p, sizeof(p), "%s/outputs", outdir);
    mkdir_p(p);
    pj(p, sizeof(p), "%s/runs.csv", outdir);
    int fresh = !path_exists(p);
    FILE *csv = fopen(p, "a");
    if (!csv) die("cannot open %s: %s", p, strerror(errno));
    if (fresh) fputs(CSV_HEADER, csv);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr, "toolcal %s: backend=%s condition=%s output=%s\n", TOOLCAL_VERSION, cg_mode_name(cg.mode), cond, outdir);
    int done = 0;
    for (int i = 0; i < c->n_tools && !g_stop; i++) {
        const tool_t *t = &c->tools[i];
        if (only && strcmp(only, t->name)) continue;
        int runs = runs_override > 0 ? runs_override : (t->runs > 0 ? t->runs : c->s.runs);
        int total = runs + c->s.warmup;
        long long cpu_sum = 0, wall_max = 0, mem_max = 0;
        int fails = 0;
        fprintf(stderr, "  %-24s ", t->name);
        for (int k = 0; k < total && !g_stop; k++) {
            int warm = k < c->s.warmup;
            int idx = warm ? 0 : k - c->s.warmup + 1;
            char save[PATH_LEN + 128];
            pj(save, sizeof(save), "%s/outputs/%s.txt", outdir, t->name);
            result_t r;
            run_once(c, &cg, t, idx, (idx == 1) ? save : NULL, &r);
            fprintf(csv, "%s,%s,%s,%d,%d,%d,%d,%d,%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%d,%s\n",
                    t->name, t->plane, cond, idx, warm, r.rc, r.sig, r.timed_out, r.capped, r.wall_ms, r.cpu_us / 1000,
                    r.ru_cpu_us / 1000, r.mem_peak_b >= 0 ? r.mem_peak_b / 1024 : -1, r.pids_peak,
                    r.rbytes >= 0 ? r.rbytes / 1024 : -1, r.wbytes >= 0 ? r.wbytes / 1024 : -1, r.out_bytes,
                    r.induced_ms, r.other_ms, r.psi_cpu_us, r.psi_mem_us, r.leftover, cg_mode_name(cg.mode));
            fflush(csv);
            if (!warm) {
                cpu_sum += r.cpu_us / 1000 + r.induced_ms;
                if (r.wall_ms > wall_max) wall_max = r.wall_ms;
                if (r.mem_peak_b / 1024 > mem_max) mem_max = r.mem_peak_b / 1024;
                if (r.rc != 0 || r.timed_out || r.capped) fails++;
            }
            fputc(warm ? 'w' : (r.rc == 0 && !r.timed_out ? '.' : 'x'), stderr);
            if (c->s.cooldown_ms > 0) usleep((useconds_t)c->s.cooldown_ms * 1000);
        }
        fprintf(stderr, "  avg cpu %lld ms, max wall %lld ms, max mem %lld KB%s\n", runs ? cpu_sum / runs : 0,
                wall_max, mem_max, fails ? "  (failures)" : "");
        done++;
    }
    fclose(csv);
    cg_cleanup_base(&cg);
    if (only && !done) die("tool \"%s\" not found in catalog", only);
    fprintf(stderr, "raw results: %s/runs.csv\n", outdir);
    return g_stop ? 130 : 0;
}

/* ------------------------------------------------------------------ */
/* Subcommand: report                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char tool[64], plane[32], cond[32];
    int warmup, rc, timed_out, capped, leftover;
    double wall, cpu, induced, mem, pids, io, out_kb;
} rec_t;

typedef struct {
    rec_t *v;
    size_t n, cap;
} recs_t;

static int split_csv(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        f[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = 0;
        p = c + 1;
    }
    char *nl = strpbrk(f[n - 1], "\r\n");
    if (nl) *nl = 0;
    return n;
}

static int col(char **hdr, int nh, const char *name)
{
    for (int i = 0; i < nh; i++)
        if (!strcmp(hdr[i], name)) return i;
    return -1;
}

static void load_csv(const char *path, recs_t *R)
{
    FILE *f = fopen(path, "r");
    if (!f) die("cannot read %s: %s", path, strerror(errno));
    char hline[1024], line[1024];
    if (!fgets(hline, sizeof(hline), f)) { fclose(f); return; }
    char *h[64];
    int nh = split_csv(hline, h, 64);
    int ci[13];
    const char *need[] = {"tool", "plane", "condition", "warmup", "rc", "timed_out", "output_capped",
                          "wall_ms", "cpu_ms", "induced_cpu_ms", "mem_kb", "out_bytes", "leftover_procs"};
    for (int i = 0; i < 13; i++)
        if ((ci[i] = col(h, nh, need[i])) < 0) die("%s: column %s missing", path, need[i]);
    int c_pids = col(h, nh, "pids"), c_rk = col(h, nh, "read_kb"), c_wk = col(h, nh, "write_kb");

    while (fgets(line, sizeof(line), f)) {
        char *v[64];
        int nv = split_csv(line, v, 64);
        if (nv < nh) continue;
        if (R->n == R->cap) {
            R->cap = R->cap ? R->cap * 2 : 256;
            R->v = realloc(R->v, R->cap * sizeof(rec_t));
            if (!R->v) die("out of memory");
        }
        rec_t *r = &R->v[R->n++];
        memset(r, 0, sizeof(*r));
        safe_copy(r->tool, sizeof(r->tool), v[ci[0]]);
        safe_copy(r->plane, sizeof(r->plane), v[ci[1]]);
        safe_copy(r->cond, sizeof(r->cond), v[ci[2]]);
        r->warmup = atoi(v[ci[3]]);
        r->rc = atoi(v[ci[4]]);
        r->timed_out = atoi(v[ci[5]]);
        r->capped = atoi(v[ci[6]]);
        r->wall = atof(v[ci[7]]);
        r->cpu = atof(v[ci[8]]);
        r->induced = atof(v[ci[9]]);
        r->mem = atof(v[ci[10]]);
        r->out_kb = atof(v[ci[11]]) / 1024.0;
        r->leftover = atoi(v[ci[12]]);
        r->pids = c_pids >= 0 ? atof(v[c_pids]) : -1;
        double rk = c_rk >= 0 ? atof(v[c_rk]) : -1, wk = c_wk >= 0 ? atof(v[c_wk]) : -1;
        r->io = (rk >= 0 && wk >= 0) ? rk + wk : -1;
    }
    fclose(f);
}

typedef struct {
    double p50, p95, max, mean;
    int n;
} stat_t;

static int dcmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Nearest-rank percentiles; ignores negative values (= not measured). */
static stat_t stats(double *v, int n)
{
    stat_t s = {0, 0, 0, 0, 0};
    double *w = malloc(sizeof(double) * (size_t)(n ? n : 1));
    if (!w) die("out of memory");
    int m = 0;
    for (int i = 0; i < n; i++)
        if (v[i] >= 0) w[m++] = v[i];
    s.n = m;
    if (m) {
        qsort(w, (size_t)m, sizeof(double), dcmp);
        double sum = 0;
        for (int i = 0; i < m; i++) sum += w[i];
        s.mean = sum / m;
        s.p50 = w[(int)ceil(0.50 * m) - 1];
        s.p95 = w[(int)ceil(0.95 * m) - 1];
        s.max = w[m - 1];
    }
    free(w);
    return s;
}

enum { M_CPU_TOTAL, M_CPU_SELF, M_INDUCED, M_WALL, M_MEM, M_OUT, M_PIDS, M_IO, N_METRICS };
static const char *metric_key[N_METRICS] = {"cpu_total_ms", "cpu_self_ms", "induced_cpu_ms", "wall_ms",
                                            "mem_kb",       "out_kb",      "pids",           "io_kb"};

typedef struct {
    char cond[32];
    int runs, nonzero_rc, timeouts, capped, leftover_runs;
    stat_t m[N_METRICS];
} condstat_t;

#define MAX_CONDS 8

typedef struct {
    char name[64], plane[32], declared[32];
    condstat_t cs[MAX_CONDS];
    int n_cs;
    double b_cpu, b_wall, b_mem, b_out;
    char basis_cond[32];
    const char *klass;
    const char *verdict;
    char notes[1024];
} toolrep_t;

static double round_up(double v, double step) { return ceil(v / step) * step; }

static void add_note(toolrep_t *t, const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (t->notes[0]) strncat(t->notes, "; ", sizeof(t->notes) - strlen(t->notes) - 1);
    strncat(t->notes, b, sizeof(t->notes) - strlen(t->notes) - 1);
}

static void json_escape(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fprintf(f, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", *s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

static int cmd_report(const config_t *c, const char *prefix, const char *basis, char **files, int nfiles)
{
    recs_t R = {0};
    for (int i = 0; i < nfiles; i++) load_csv(files[i], &R);
    if (!R.n) die("no measurements found in input CSV files");

    /* Tool order: catalog first, then anything else seen in the CSVs. */
    static toolrep_t T[MAX_TOOLS];
    int nt = 0;
    for (int i = 0; i < c->n_tools && nt < MAX_TOOLS; i++) {
        memset(&T[nt], 0, sizeof(T[nt]));
        safe_copy(T[nt].name, sizeof(T[nt].name), c->tools[i].name);
        safe_copy(T[nt].plane, sizeof(T[nt].plane), c->tools[i].plane);
        safe_copy(T[nt].declared, sizeof(T[nt].declared), c->tools[i].declared_class);
        nt++;
    }
    for (size_t i = 0; i < R.n; i++) {
        int found = 0;
        for (int k = 0; k < nt && !found; k++) found = !strcmp(T[k].name, R.v[i].tool);
        if (!found && nt < MAX_TOOLS) {
            memset(&T[nt], 0, sizeof(T[nt]));
            safe_copy(T[nt].name, sizeof(T[nt].name), R.v[i].tool);
            safe_copy(T[nt].plane, sizeof(T[nt].plane), R.v[i].plane);
            nt++;
        }
    }
    /* Condition order: first appearance. */
    char conds[MAX_CONDS][32];
    int nc = 0;
    for (size_t i = 0; i < R.n; i++) {
        int found = 0;
        for (int k = 0; k < nc && !found; k++) found = !strcmp(conds[k], R.v[i].cond);
        if (!found && nc < MAX_CONDS) safe_copy(conds[nc++], 32, R.v[i].cond);
    }

    double *buf[N_METRICS];
    for (int m = 0; m < N_METRICS; m++) {
        buf[m] = malloc(sizeof(double) * R.n);
        if (!buf[m]) die("out of memory");
    }

    int n_measured = 0;
    for (int ti = 0; ti < nt; ti++) {
        toolrep_t *t = &T[ti];
        for (int ci = 0; ci < nc; ci++) {
            condstat_t *cs = &t->cs[t->n_cs];
            memset(cs, 0, sizeof(*cs));
            safe_copy(cs->cond, sizeof(cs->cond), conds[ci]);
            int n = 0;
            for (size_t i = 0; i < R.n; i++) {
                const rec_t *r = &R.v[i];
                if (r->warmup || strcmp(r->tool, t->name) || strcmp(r->cond, conds[ci])) continue;
                buf[M_CPU_TOTAL][n] = r->cpu + (r->induced > 0 ? r->induced : 0);
                buf[M_CPU_SELF][n] = r->cpu;
                buf[M_INDUCED][n] = r->induced;
                buf[M_WALL][n] = r->wall;
                buf[M_MEM][n] = r->mem;
                buf[M_OUT][n] = r->out_kb;
                buf[M_PIDS][n] = r->pids;
                buf[M_IO][n] = r->io;
                n++;
                cs->nonzero_rc += r->rc != 0;
                cs->timeouts += r->timed_out;
                cs->capped += r->capped;
                cs->leftover_runs += r->leftover > 0;
            }
            if (!n) continue;
            cs->runs = n;
            for (int m = 0; m < N_METRICS; m++) cs->m[m] = stats(buf[m], n);
            t->n_cs++;
        }
        if (!t->n_cs) { t->verdict = "NOT MEASURED"; t->klass = "-"; continue; }
        n_measured++;

        /* Budget basis: a named condition, or the worst p95 across conditions. */
        double p_cpu = 0, p_wall = 0, p_mem = 0, p_out = 0;
        int used = 0;
        for (int k = 0; k < t->n_cs; k++) {
            condstat_t *cs = &t->cs[k];
            if (basis && strcmp(basis, "max") && strcmp(basis, cs->cond)) continue;
            used = 1;
            if (cs->m[M_CPU_TOTAL].p95 > p_cpu) p_cpu = cs->m[M_CPU_TOTAL].p95;
            if (cs->m[M_WALL].p95 > p_wall) p_wall = cs->m[M_WALL].p95;
            if (cs->m[M_MEM].p95 > p_mem) p_mem = cs->m[M_MEM].p95;
            if (cs->m[M_OUT].p95 > p_out) p_out = cs->m[M_OUT].p95;
        }
        if (!used) {
            add_note(t, "basis condition '%s' not measured; used worst of all conditions", basis);
            for (int k = 0; k < t->n_cs; k++) {
                condstat_t *cs = &t->cs[k];
                if (cs->m[M_CPU_TOTAL].p95 > p_cpu) p_cpu = cs->m[M_CPU_TOTAL].p95;
                if (cs->m[M_WALL].p95 > p_wall) p_wall = cs->m[M_WALL].p95;
                if (cs->m[M_MEM].p95 > p_mem) p_mem = cs->m[M_MEM].p95;
                if (cs->m[M_OUT].p95 > p_out) p_out = cs->m[M_OUT].p95;
            }
        }
        safe_copy(t->basis_cond, sizeof(t->basis_cond), (basis && used) ? basis : "max");
        double f = c->s.budget_factor;
        t->b_cpu = fmax(c->s.min_cpu_ms, round_up(p_cpu * f, 10));
        t->b_wall = fmax(c->s.min_wall_ms, round_up(p_wall * f, 100));
        t->b_mem = fmax(c->s.min_mem_kb, round_up(p_mem * f, 256));
        t->b_out = fmax(c->s.min_out_kb, round_up(p_out * f, 1));

        t->klass = NULL;
        for (int k = 0; k < c->n_classes && !t->klass; k++) {
            const class_t *cl = &c->classes[k];
            if (t->b_cpu <= cl->cpu_ms && t->b_wall <= cl->wall_ms && t->b_mem <= cl->mem_kb && t->b_out <= cl->out_kb)
                t->klass = cl->name;
        }
        int fail = 0, review = 0, min_runs = 1 << 30;
        if (!t->klass) {
            t->klass = "unclassified";
            fail = 1;
            const class_t *top = &c->classes[c->n_classes - 1];
            if (t->b_cpu > top->cpu_ms) add_note(t, "CPU budget %.0f ms exceeds %s limit %.0f ms", t->b_cpu, top->name, top->cpu_ms);
            if (t->b_wall > top->wall_ms) add_note(t, "wall budget %.0f ms exceeds %s limit %.0f ms", t->b_wall, top->name, top->wall_ms);
            if (t->b_mem > top->mem_kb) add_note(t, "memory budget %.0f KB exceeds %s limit %.0f KB", t->b_mem, top->name, top->mem_kb);
            if (t->b_out > top->out_kb) add_note(t, "output budget %.0f KB exceeds %s limit %.0f KB", t->b_out, top->name, top->out_kb);
        }
        if (t->declared[0] && strcmp(t->declared, t->klass)) {
            add_note(t, "declared class '%s' but measured '%s'", t->declared, t->klass);
            review = 1;
        }
        for (int k = 0; k < t->n_cs; k++) {
            condstat_t *cs = &t->cs[k];
            if (cs->timeouts) { add_note(t, "%d timeout(s) in %s", cs->timeouts, cs->cond); fail = 1; }
            if (cs->capped) { add_note(t, "output hard-capped %d time(s) in %s", cs->capped, cs->cond); fail = 1; }
            if (cs->nonzero_rc) { add_note(t, "non-zero exit in %d/%d runs (%s)", cs->nonzero_rc, cs->runs, cs->cond); review = 1; }
            if (cs->leftover_runs) { add_note(t, "left processes behind in %d run(s) (%s)", cs->leftover_runs, cs->cond); review = 1; }
            stat_t *tot = &cs->m[M_CPU_TOTAL], *ind = &cs->m[M_INDUCED];
            if (tot->p95 > 20 && ind->p95 > 0.5 * tot->p95) {
                add_note(t, "most CPU is induced in other daemons (%s: %.0f of %.0f ms)", cs->cond, ind->p95, tot->p95);
                review = 1;
            }
            if (cs->m[M_WALL].p50 > 50 && cs->m[M_WALL].p95 > 3 * cs->m[M_WALL].p50) {
                add_note(t, "unstable wall time in %s (p95 %.0f vs p50 %.0f ms)", cs->cond, cs->m[M_WALL].p95, cs->m[M_WALL].p50);
                review = 1;
            }
            if (cs->runs < min_runs) min_runs = cs->runs;
        }
        if (min_runs < 20) add_note(t, "fewer than 20 runs in a condition (min %d); p95 is approximate", min_runs);
        t->verdict = fail ? "FAIL" : review ? "REVIEW" : "PASS";
    }
    if (!n_measured) die("none of the catalog tools appear in the CSV files");

    /* ---- write CSV summary ---- */
    char p[PATH_LEN + 16];
    pj(p, sizeof(p), "%s.csv", prefix);
    FILE *fc = fopen(p, "w");
    if (!fc) die("cannot write %s: %s", p, strerror(errno));
    fputs("tool,plane,class,verdict,budget_cpu_ms,budget_wall_ms,budget_mem_kb,budget_out_kb,basis", fc);
    for (int k = 0; k < nc; k++) fprintf(fc, ",%s_cpu_p95,%s_wall_p95,%s_mem_p95", conds[k], conds[k], conds[k]);
    fputs(",notes\n", fc);
    for (int i = 0; i < nt; i++) {
        toolrep_t *t = &T[i];
        if (!t->n_cs) continue;
        fprintf(fc, "%s,%s,%s,%s,%.0f,%.0f,%.0f,%.0f,%s", t->name, t->plane, t->klass, t->verdict, t->b_cpu, t->b_wall,
                t->b_mem, t->b_out, t->basis_cond);
        for (int k = 0; k < nc; k++) {
            condstat_t *cs = NULL;
            for (int j = 0; j < t->n_cs; j++)
                if (!strcmp(t->cs[j].cond, conds[k])) cs = &t->cs[j];
            if (cs) fprintf(fc, ",%.0f,%.0f,%.0f", cs->m[M_CPU_TOTAL].p95, cs->m[M_WALL].p95, cs->m[M_MEM].p95);
            else fputs(",,,", fc);
        }
        char nb[1024];
        safe_copy(nb, sizeof(nb), t->notes);
        for (char *q = nb; *q; q++)
            if (*q == ',') *q = ' ';
        fprintf(fc, ",%s\n", nb);
    }
    fclose(fc);

    /* ---- write JSON (catalog-ready budgets) ---- */
    pj(p, sizeof(p), "%s.json", prefix);
    FILE *fj = fopen(p, "w");
    if (!fj) die("cannot write %s: %s", p, strerror(errno));
    time_t now = time(NULL);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", localtime(&now));
    struct utsname un;
    uname(&un);
    fprintf(fj, "{\n  \"generated\": \"%s\",\n  \"host\": ", ts);
    json_escape(fj, un.nodename);
    fprintf(fj, ",\n  \"kernel\": ");
    json_escape(fj, un.release);
    fprintf(fj, ",\n  \"budget_factor\": %.2f,\n  \"basis\": ", c->s.budget_factor);
    json_escape(fj, basis ? basis : "max");
    fprintf(fj, ",\n  \"tools\": [\n");
    int first = 1;
    for (int i = 0; i < nt; i++) {
        toolrep_t *t = &T[i];
        if (!t->n_cs) continue;
        fprintf(fj, "%s    {\n      \"name\": ", first ? "" : ",\n");
        first = 0;
        json_escape(fj, t->name);
        fprintf(fj, ",\n      \"plane\": ");
        json_escape(fj, t->plane);
        fprintf(fj, ",\n      \"class\": ");
        json_escape(fj, t->klass);
        fprintf(fj, ",\n      \"verdict\": ");
        json_escape(fj, t->verdict);
        fprintf(fj, ",\n      \"budget\": { \"cpu_ms\": %.0f, \"wall_ms\": %.0f, \"mem_kb\": %.0f, \"out_kb\": %.0f },\n",
                t->b_cpu, t->b_wall, t->b_mem, t->b_out);
        fprintf(fj, "      \"conditions\": {");
        for (int k = 0; k < t->n_cs; k++) {
            condstat_t *cs = &t->cs[k];
            fprintf(fj, "%s\n        ", k ? "," : "");
            json_escape(fj, cs->cond);
            fprintf(fj, ": { \"runs\": %d, \"nonzero_rc\": %d, \"timeouts\": %d, \"output_capped\": %d, \"leftover_runs\": %d",
                    cs->runs, cs->nonzero_rc, cs->timeouts, cs->capped, cs->leftover_runs);
            for (int m = 0; m < N_METRICS; m++) {
                if (!cs->m[m].n) continue;
                fprintf(fj, ", \"%s\": { \"p50\": %.1f, \"p95\": %.1f, \"max\": %.1f }", metric_key[m], cs->m[m].p50,
                        cs->m[m].p95, cs->m[m].max);
            }
            fprintf(fj, " }");
        }
        fprintf(fj, "\n      },\n      \"notes\": ");
        json_escape(fj, t->notes);
        fprintf(fj, "\n    }");
    }
    fprintf(fj, "\n  ]\n}\n");
    fclose(fj);

    /* ---- write Markdown report ---- */
    pj(p, sizeof(p), "%s.md", prefix);
    FILE *fm = fopen(p, "w");
    if (!fm) die("cannot write %s: %s", p, strerror(errno));
    int npass = 0, nrev = 0, nfail = 0;
    for (int i = 0; i < nt; i++) {
        if (!T[i].n_cs) continue;
        if (!strcmp(T[i].verdict, "PASS")) npass++;
        else if (!strcmp(T[i].verdict, "REVIEW")) nrev++;
        else nfail++;
    }
    fprintf(fm, "# Tool threshold calibration report\n\n");
    fprintf(fm, "%d tools measured: **%d pass, %d need review, %d fail**. Budgets are p95 x %.2f of the %s, "
                "with CPU including time induced in other daemons.\n\n",
            n_measured, npass, nrev, nfail, c->s.budget_factor,
            basis && strcmp(basis, "max") ? "named basis condition" : "worst condition");
    fprintf(fm, "- Generated: %s\n- Host: %s, kernel %s\n- Conditions:", ts, un.nodename, un.release);
    for (int k = 0; k < nc; k++) fprintf(fm, " %s%s", conds[k], k + 1 < nc ? "," : "\n");
    fprintf(fm, "- Input: ");
    for (int i = 0; i < nfiles; i++) fprintf(fm, "`%s`%s", files[i], i + 1 < nfiles ? ", " : "\n\n");

    fprintf(fm, "## Summary\n\n| Tool | Plane | Class | Verdict | CPU budget (ms) | Wall budget (ms) | Memory budget (KB) | Output budget (KB) |\n");
    fprintf(fm, "| --- | --- | --- | --- | --- | --- | --- | --- |\n");
    for (int i = 0; i < nt; i++) {
        toolrep_t *t = &T[i];
        if (!t->n_cs) continue;
        fprintf(fm, "| %s | %s | %s | %s | %.0f | %.0f | %.0f | %.0f |\n", t->name, t->plane, t->klass, t->verdict,
                t->b_cpu, t->b_wall, t->b_mem, t->b_out);
    }
    fprintf(fm, "\n## Per-tool detail\n\nValues are p50 / p95 / max over the measured runs; warm-up runs are excluded.\n");
    for (int i = 0; i < nt; i++) {
        toolrep_t *t = &T[i];
        if (!t->n_cs) continue;
        fprintf(fm, "\n### %s (%s, %s)\n\n", t->name, t->klass, t->verdict);
        for (int k = 0; k < c->n_tools; k++)
            if (!strcmp(c->tools[k].name, t->name)) fprintf(fm, "Command: `%s`\n\n", c->tools[k].command);
        fprintf(fm, "| Condition | Runs | CPU total (ms) | CPU self (ms) | Induced CPU (ms) | Wall (ms) | Memory (KB) | Output (KB) | Tasks | Failures |\n");
        fprintf(fm, "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n");
        for (int k = 0; k < t->n_cs; k++) {
            condstat_t *cs = &t->cs[k];
            fprintf(fm, "| %s | %d ", cs->cond, cs->runs);
            for (int m = 0; m < N_METRICS; m++) {
                if (m == M_IO) continue;
                if (!cs->m[m].n) { fprintf(fm, "| n/m "); continue; }
                if (m == M_PIDS) fprintf(fm, "| %.0f ", cs->m[m].max);
                else if (m == M_OUT) fprintf(fm, "| %.1f / %.1f / %.1f ", cs->m[m].p50, cs->m[m].p95, cs->m[m].max);
                else fprintf(fm, "| %.0f / %.0f / %.0f ", cs->m[m].p50, cs->m[m].p95, cs->m[m].max);
            }
            fprintf(fm, "| rc!=0: %d, timeout: %d, capped: %d, leftover: %d |\n", cs->nonzero_rc, cs->timeouts,
                    cs->capped, cs->leftover_runs);
        }
        if (t->notes[0]) fprintf(fm, "\nNotes: %s.\n", t->notes);
    }
    fprintf(fm, "\n## Class limits used\n\n| Class | CPU (ms) | Wall (ms) | Memory (KB) | Output (KB) |\n| --- | --- | --- | --- | --- |\n");
    for (int k = 0; k < c->n_classes; k++)
        fprintf(fm, "| %s | %.0f | %.0f | %.0f | %.0f |\n", c->classes[k].name, c->classes[k].cpu_ms, c->classes[k].wall_ms,
                c->classes[k].mem_kb, c->classes[k].out_kb);
    fprintf(fm, "\nVerdicts: PASS = fits a class with no issues. REVIEW = fits a class but has exit errors, leftover "
                "processes, mostly induced CPU, unstable timing or a class different from the one declared. FAIL = "
                "exceeds every class, timed out, or hit the output hard cap.\n");
    fclose(fm);

    printf("%-24s %-12s %-8s %8s %8s %9s %7s\n", "TOOL", "CLASS", "VERDICT", "CPU_ms", "WALL_ms", "MEM_KB", "OUT_KB");
    for (int i = 0; i < nt; i++) {
        toolrep_t *t = &T[i];
        if (!t->n_cs) continue;
        printf("%-24s %-12s %-8s %8.0f %8.0f %9.0f %7.0f\n", t->name, t->klass, t->verdict, t->b_cpu, t->b_wall, t->b_mem,
               t->b_out);
    }
    printf("\nreport: %s.md  %s.json  %s.csv\n", prefix, prefix, prefix);
    for (int m = 0; m < N_METRICS; m++) free(buf[m]);
    free(R.v);
    return nfail ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Subcommand: hog (synthetic load for the stressed condition)         */
/* ------------------------------------------------------------------ */

static pid_t g_hogs[256];
static int g_nhogs = 0;

static int cmd_hog(int ncpu, long mem_mb, int seconds)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    for (int i = 0; i < ncpu && g_nhogs < 255; i++) {
        pid_t p = fork();
        if (p == 0) {
            volatile unsigned long x = 0;
            for (;;) x++;
        }
        if (p > 0) g_hogs[g_nhogs++] = p;
    }
    if (mem_mb > 0) {
        pid_t p = fork();
        if (p == 0) {
            size_t sz = (size_t)mem_mb * 1024 * 1024;
            char *m = malloc(sz);
            if (!m) _exit(1);
            long pg = sysconf(_SC_PAGESIZE);
            for (;;) {
                for (size_t o = 0; o < sz; o += (size_t)pg) m[o]++;
                sleep(1);
            }
        }
        if (p > 0) g_hogs[g_nhogs++] = p;
    }
    fprintf(stderr, "toolcal hog: %d cpu spinner(s), %ld MB resident; %s\n", ncpu, mem_mb,
            seconds > 0 ? "timed" : "until SIGTERM");
    long long end = now_ms() + (long long)seconds * 1000;
    while (!g_stop && (seconds <= 0 || now_ms() < end)) usleep(200000);
    for (int i = 0; i < g_nhogs; i++) kill(g_hogs[i], SIGKILL);
    for (int i = 0; i < g_nhogs; i++) waitpid(g_hogs[i], NULL, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
            "toolcal %s - tool resource calibration\n\n"
            "  toolcal list   -c tools.json\n"
            "  toolcal run    -c tools.json -o <dir> [-l condition] [-n runs] [-t tool]\n"
            "  toolcal report -c tools.json -o <prefix> [-b basis|max] runs.csv [runs.csv ...]\n"
            "  toolcal hog    [--cpu N] [--mem-mb M] [--seconds S]\n",
            TOOLCAL_VERSION);
    exit(2);
}

int main(int argc, char **argv)
{
    if (argc < 2) usage();
    const char *sub = argv[1];
    const char *cfg = NULL, *out = NULL, *cond = "idle", *only = NULL, *basis = NULL;
    int runs = 0, hog_cpu = 1, hog_secs = 0;
    long hog_mem = 0;
    char *files[64];
    int nfiles = 0;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "-c") && v) { cfg = v; i++; }
        else if (!strcmp(a, "-o") && v) { out = v; i++; }
        else if (!strcmp(a, "-l") && v) { cond = v; i++; }
        else if (!strcmp(a, "-n") && v) { runs = atoi(v); i++; }
        else if (!strcmp(a, "-t") && v) { only = v; i++; }
        else if (!strcmp(a, "-b") && v) { basis = v; i++; }
        else if (!strcmp(a, "--cpu") && v) { hog_cpu = atoi(v); i++; }
        else if (!strcmp(a, "--mem-mb") && v) { hog_mem = atol(v); i++; }
        else if (!strcmp(a, "--seconds") && v) { hog_secs = atoi(v); i++; }
        else if (a[0] != '-' && nfiles < 64) files[nfiles++] = argv[i];
        else usage();
    }
    if (!strcmp(sub, "hog")) return cmd_hog(hog_cpu, hog_mem, hog_secs);
    if (!cfg) usage();
    static config_t c;
    load_config(cfg, &c);
    if (!strcmp(sub, "list")) return cmd_list(&c);
    if (!strcmp(sub, "run")) {
        if (!out) usage();
        char cl[32];
        safe_copy(cl, sizeof(cl), cond);
        sanitize_name(cl);
        return cmd_run(&c, out, cl, runs, only);
    }
    if (!strcmp(sub, "report")) {
        if (!out || !nfiles) usage();
        return cmd_report(&c, out, basis, files, nfiles);
    }
    usage();
    return 2;
}

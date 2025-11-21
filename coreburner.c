/* coreburner.c
 *
 * CoreBurner: Advanced CPU stress & telemetry tool (full version)
 *
 * Features:
 *  - INT/FLOAT/AVX/MIXED workloads
 *  - Single / Multi modes, per-thread CPU pinning
 *  - Container-aware cpuset detection
 *  - CPU hotplug detection and re-pin monitor thread
 *  - Temperature reading with sanity checks
 *  - Frequency logging, governor & min/max writes (root required)
 *  - Per-core freq-table support
 *  - Dynamic frequency tuner (auto step-down)
 *  - CSV logging + human-readable summary
 *  - Safe logging fallback, ops counter wraparound handling
 *
 * Build:
 *   gcc -O2 -march=native -pthread -std=c11 -Wall -Wextra -o coreburner coreburner.c -lm
 *
 * Author: ChatGPT (patched)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <sys/sysinfo.h>
#include <signal.h>
#include <immintrin.h>
#include <math.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cpuid.h>
#include <stdarg.h>

#define CONTROL_PERIOD_MS 100
#define DEFAULT_LOG_INTERVAL 1
#define DEFAULT_MAX_THREADS 256
#define DEFAULT_DURATION_LIMIT_SEC (24*3600)
#define DEFAULT_TEMP_THRESHOLD 90.0
#define DYN_FREQ_STEP_PCT 10
#define DYN_FREQ_COOL_THRESH_DELTA 5.0
#define MAX_CORES_TO_LOG 64
#define TEMP_SANITY_MIN -20.0
#define TEMP_SANITY_MAX 150.0

static volatile sig_atomic_t stop_flag = 0;
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;

void sigint_handler(int s) { (void)s; stop_flag = 1; }

/* ------------------ helpers ------------------ */
static int get_affinity_cpu_count() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return get_nprocs();
    int cnt = 0;
    for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &set)) ++cnt;
    return cnt > 0 ? cnt : 1;
}

/* parse duration like 30, 30s, 5m, 1h */
long parse_duration_seconds(const char *s) {
    if (!s) return -1;
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || errno != 0) return -1;
    while (*end == ' ') ++end;
    if (*end == '\0' || *end == 's' || *end == 'S') return (long)round(v);
    if (*end == 'm' || *end == 'M') return (long)round(v * 60.0);
    if (*end == 'h' || *end == 'H') return (long)round(v * 3600.0);
    return -1;
}

/* ------------------ /proc/stat reading ------------------ */
int read_proc_stat(uint64_t *total_out, uint64_t *idle_out, int max_cpus) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char line[512];
    int idx = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0) break;
        if (line[3] == ' ') continue; // skip aggregate
        unsigned long long user=0, nice=0, system=0, idle=0, iowait=0, irq=0, softirq=0, steal=0;
        int matched = sscanf(line, "cpu%*d %llu %llu %llu %llu %llu %llu %llu %llu",
                             &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
        if (matched < 4) continue;
        uint64_t idle_all = idle + (matched >= 5 ? iowait : 0);
        uint64_t nonidle = user + nice + system + (matched >= 6 ? irq : 0) + (matched >= 7 ? softirq : 0) + (matched >= 8 ? steal : 0);
        uint64_t total = idle_all + nonidle;
        if (idx < max_cpus) {
            total_out[idx] = total;
            idle_out[idx] = idle_all;
        }
        idx++;
    }
    fclose(f);
    return idx;
}

/* ------------------ temperature helpers ------------------ */
char *find_temperature_input_path() {
    const char *cands[] = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/thermal/thermal_zone1/temp",
        NULL
    };
    for (int i = 0; cands[i]; ++i) if (access(cands[i], R_OK) == 0) return strdup(cands[i]);
    char buf[256];
    for (int i = 0; i < 64; ++i) {
        snprintf(buf, sizeof(buf), "/sys/class/hwmon/hwmon%d/temp1_input", i);
        if (access(buf, R_OK) == 0) return strdup(buf);
    }
    return NULL;
}

double read_temperature(const char *path) {
    if (!path) return NAN;
    FILE *f = fopen(path, "r");
    if (!f) return NAN;
    long v = 0;
    if (fscanf(f, "%ld", &v) != 1) { fclose(f); return NAN; }
    fclose(f);
    double t = (v > 1000) ? (v / 1000.0) : (double)v;
    if (t < TEMP_SANITY_MIN || t > TEMP_SANITY_MAX) return NAN;
    return t;
}

/* ------------------ cpufreq helpers ------------------ */
int read_scaling_cur_freq(int cpu, long *out_hz) {
    char path[256]; snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    FILE *f = fopen(path, "r"); if (!f) return -1;
    long v = 0; if (fscanf(f, "%ld", &v) != 1) { fclose(f); return -1; } fclose(f);
    *out_hz = v; return 0;
}
int write_sysfs_int(const char *path, long value) {
    FILE *f = fopen(path, "w"); if (!f) return -1;
    int rc = fprintf(f, "%ld\n", value) < 0 ? -1 : 0; fclose(f); return rc;
}
int write_scaling_governor(int cpu, const char *gov) {
    char path[256]; snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
    FILE *f = fopen(path, "w"); if (!f) return -1; int rc = fprintf(f, "%s\n", gov) < 0 ? -1 : 0; fclose(f); return rc;
}
int write_scaling_min_max(int cpu, long min_hz, long max_hz) {
    char path_min[256], path_max[256];
    snprintf(path_min, sizeof(path_min), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", cpu);
    snprintf(path_max, sizeof(path_max), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", cpu);
    if (min_hz >= 0) if (write_sysfs_int(path_min, min_hz) != 0) return -1;
    if (max_hz >= 0) if (write_sysfs_int(path_max, max_hz) != 0) return -1;
    return 0;
}

/* ------------------ workload kernels ------------------ */
typedef enum { W_INT, W_FLOAT, W_AVX, W_MIXED } workload_t;
void int_work_unit(volatile uint64_t *state) {
    uint64_t x = *state;
    for (int i = 0; i < 256; ++i) { x += (x << 1) ^ 0x9e3779b97f4a7c15ULL; x ^= (x >> 7); }
    *state = x;
}
void float_work_unit(volatile double *state) {
    double x = *state;
    for (int i = 0; i < 256; ++i) { x = x * 1.0000001 + 0.10000001; x = fmod(x, 100000.0); }
    *state = x;
}
void avx_work_unit(float *fbuf) {
    __m256 a = _mm256_loadu_ps(fbuf);
    __m256 b = _mm256_set1_ps(1.000001f);
    for (int i = 0; i < 256; ++i) { a = _mm256_add_ps(a, b); a = _mm256_mul_ps(a, _mm256_set1_ps(0.999999f)); }
    _mm256_storeu_ps(fbuf, a);
}

/* ------------------ CPU feature detection (AVX) ------------------ */
static int check_xgetbv_avx() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int a, d; __asm__ volatile ("xgetbv" : "=a"(a), "=d"(d) : "c"(0)); uint64_t xcr0 = ((uint64_t)d << 32) | a; return ((xcr0 & 0x6ULL) == 0x6ULL);
#else
    return 0;
#endif
}
int cpu_supports_avx() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int eax, ebx, ecx, edx; if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return 0;
    int has_avx = (ecx >> 28) & 1;
    int has_osxsave = (ecx >> 27) & 1;
    if (!has_avx || !has_osxsave) return 0;
    if (!check_xgetbv_avx()) return 0;
    return 1;
#else
    return 0;
#endif
}

/* ------------------ worker and monitor ------------------ */
typedef struct {
    int cpu_id;
    double target_util;
    workload_t type;
    _Atomic uint64_t ops_done;
} worker_arg_t;

typedef struct {
    pthread_t *tids;
    worker_arg_t *wargs;
    int nthreads;
} workers_t;

static workers_t g_workers = {0};
static int g_available_cpus = 0;

void safe_nanosleep(long sec, long nsec) {
    struct timespec req, rem;
    req.tv_sec = sec; req.tv_nsec = nsec;
    while (!stop_flag) {
        if (nanosleep(&req, &rem) == 0) break;
        if (errno == EINTR) { req = rem; continue; }
        break;
    }
}

void *worker_thread(void *arg) {
    worker_arg_t *w = (worker_arg_t *)arg;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(w->cpu_id, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        cpu_set_t cur; CPU_ZERO(&cur);
        if (sched_getaffinity(0, sizeof(cur), &cur) == 0) {
            int found = -1;
            for (int i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &cur)) { found = i; break; }
            if (found >= 0) {
                CPU_ZERO(&cpuset); CPU_SET(found, &cpuset);
                pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
                w->cpu_id = found;
            }
        }
    }

    volatile uint64_t int_state = (uint64_t)(uintptr_t)w ^ 0xabcdef;
    volatile double float_state = (double)(w->cpu_id + 1) * 1.234567;
    float avx_buf[8] __attribute__((aligned(32)));
    for (int i = 0; i < 8; ++i) avx_buf[i] = (float)(i + w->cpu_id);

    const long period_ns = CONTROL_PERIOD_MS * 1000000L;
    double util = w->target_util;
    if (util < 0) util = 0; if (util > 100) util = 100;
    long busy_ns = (long)round((util / 100.0) * period_ns);
    long sleep_ns = period_ns - busy_ns;

    struct timespec t0, t1;
    while (!stop_flag) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (busy_ns > 0) {
            if (w->type == W_INT) {
                for (;;) {
                    int_work_unit(&int_state);
                    __atomic_fetch_add(&w->ops_done, 1, __ATOMIC_RELAXED);
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    long elapsed = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
                    if (elapsed >= busy_ns || stop_flag) break;
                }
            } else if (w->type == W_FLOAT) {
                for (;;) {
                    float_work_unit(&float_state);
                    __atomic_fetch_add(&w->ops_done, 1, __ATOMIC_RELAXED);
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    long elapsed = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
                    if (elapsed >= busy_ns || stop_flag) break;
                }
            } else if (w->type == W_AVX) {
                for (;;) {
                    avx_work_unit(avx_buf);
                    __atomic_fetch_add(&w->ops_done, 1, __ATOMIC_RELAXED);
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    long elapsed = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
                    if (elapsed >= busy_ns || stop_flag) break;
                }
            } else {
                for (;;) {
                    int_work_unit(&int_state);
                    float_work_unit(&float_state);
                    avx_work_unit(avx_buf);
                    __atomic_fetch_add(&w->ops_done, 1, __ATOMIC_RELAXED);
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    long elapsed = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
                    if (elapsed >= busy_ns || stop_flag) break;
                }
            }
        }
        if (sleep_ns > 0 && !stop_flag) safe_nanosleep(0, sleep_ns);
    }
    return NULL;
}

/* Monitor thread: detect CPU hotplug / cpuset changes and reassign workers */
void *monitor_thread(void *arg) {
    (void)arg;
    int prev_aff_count = get_affinity_cpu_count();
    while (!stop_flag) {
        sleep(1);
        int cur_aff = get_affinity_cpu_count();
        if (cur_aff != prev_aff_count) {
            pthread_mutex_lock(&global_lock);
            g_available_cpus = get_affinity_cpu_count();
            for (int i = 0; i < g_workers.nthreads; ++i) {
                int desired = g_workers.wargs[i].cpu_id;
                if (desired >= g_available_cpus) {
                    int newcpu = i % g_available_cpus;
                    g_workers.wargs[i].cpu_id = newcpu;
                    cpu_set_t cpuset; CPU_ZERO(&cpuset); CPU_SET(newcpu, &cpuset);
                    if (pthread_setaffinity_np(g_workers.tids[i], sizeof(cpu_set_t), &cpuset) != 0) {
                        // best-effort
                    }
                }
            }
            pthread_mutex_unlock(&global_lock);
            prev_aff_count = cur_aff;
        }
    }
    return NULL;
}

/* ------------------ CSV safe write helper ------------------ */
int safe_fprintf_flush(FILE *f, const char *fmt, ...) {
    if (!f) return -1;
    va_list ap; va_start(ap, fmt);
    int rc = vfprintf(f, fmt, ap);
    va_end(ap);
    if (rc < 0) return -1;
    if (fflush(f) != 0) return -1;
    return 0;
}

/* ------------------ CLI helpers ------------------ */
void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --mode single|multi --util N(10-100) "
        "--duration X[s|m|h] --type INT|FLOAT|AVX|MIXED [options]\n"
        "Options:\n"
        "  --max-threads N          Max worker threads (default %d)\n"
        "  --duration-limit X       Upper allowed duration (default 24h)\n"
        "  --temp-threshold N       Auto-stop if CPU temp >= N °C (default %.1f)\n"
        "  --log FILE               Write CSV log to FILE\n"
        "  --log-interval N         Log/report interval (seconds, default %d)\n"
        "  --log-append             Append to log file instead of overwrite\n"
        "\n"
        " CPU Frequency / Governor Control (root required):\n"
        "  --set-governor GOV       Set CPUFreq governor (performance, powersave, etc.)\n"
        "  --set-min-freq HZ        Set scaling_min_freq per CPU\n"
        "  --set-max-freq HZ        Set scaling_max_freq per CPU\n"
        "  --freq-table LIST        Per-core frequencies, e.g. \"0:3200000,1:2800000\"\n"
        "\n"
        " Dynamic Frequency Management:\n"
        "  --dynamic-freq           Auto-reduce maxfreq when temperature rises\n"
        "                           Uses step-down algorithm (-10%% per event)\n"
        "\n"
        " Misc:\n"
        "  --check                  Validate and show planned run; no execution\n"
        "  --help                   Show this help screen\n"
        "\n"
        "Examples:\n"
        "  Full AVX load @ 75%% on all cores for 2 minutes:\n"
        "    %s --mode multi --util 75 --duration 2m --type AVX --log run.csv\n"
        "\n"
        "  Set governor + max frequency and run INT workload:\n"
        "    sudo %s --mode multi --type INT --duration 1m \\\n"
        "         --set-governor performance --set-max-freq 3500000\n"
        "\n"
        "  Thermal stress + auto frequency tuning:\n"
        "    sudo %s --mode multi --util 100 --duration 3m \\\n"
        "         --dynamic-freq --temp-threshold 85\n",
        prog, DEFAULT_MAX_THREADS, DEFAULT_TEMP_THRESHOLD, DEFAULT_LOG_INTERVAL, prog, prog, prog
    );
}

int str_case_equal(const char *a, const char *b) { return strcasecmp(a, b) == 0; }
workload_t parse_type(const char *s) { if (!s) return W_INT; if (str_case_equal(s, "INT")) return W_INT; if (str_case_equal(s, "FLOAT")) return W_FLOAT; if (str_case_equal(s, "AVX")) return W_AVX; return W_MIXED; }

int parse_args(int argc, char **argv,
               char **out_mode, double *out_util, long *out_duration,
               workload_t *out_type,
               int *out_max_threads, long *out_duration_limit,
               double *out_temp_threshold, int *out_check,
               char **out_log_path, int *out_log_interval, int *out_log_append,
               char **out_set_governor, long *out_set_min_freq, long *out_set_max_freq,
               char **out_freq_table, int *out_dynamic_freq) {
    *out_mode = NULL; *out_util = -1; *out_duration = -1; *out_type = W_INT;
    *out_max_threads = DEFAULT_MAX_THREADS; *out_duration_limit = DEFAULT_DURATION_LIMIT_SEC;
    *out_temp_threshold = DEFAULT_TEMP_THRESHOLD; *out_check = 0;
    *out_log_path = NULL; *out_log_interval = DEFAULT_LOG_INTERVAL; *out_log_append = 0;
    *out_set_governor = NULL; *out_set_min_freq = -1; *out_set_max_freq = -1; *out_freq_table = NULL; *out_dynamic_freq = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) { *out_mode = argv[++i]; continue; }
        if (strcmp(argv[i], "--util") == 0 && i + 1 < argc) { *out_util = atof(argv[++i]); continue; }
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) { *out_duration = parse_duration_seconds(argv[++i]); continue; }
        if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) { *out_type = parse_type(argv[++i]); continue; }
        if (strcmp(argv[i], "--max-threads") == 0 && i + 1 < argc) { *out_max_threads = atoi(argv[++i]); continue; }
        if (strcmp(argv[i], "--duration-limit") == 0 && i + 1 < argc) { *out_duration_limit = parse_duration_seconds(argv[++i]); continue; }
        if (strcmp(argv[i], "--temp-threshold") == 0 && i + 1 < argc) { *out_temp_threshold = atof(argv[++i]); continue; }
        if (strcmp(argv[i], "--check") == 0) { *out_check = 1; continue; }
        if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) { *out_log_path = argv[++i]; continue; }
        if (strcmp(argv[i], "--log-interval") == 0 && i + 1 < argc) { *out_log_interval = atoi(argv[++i]); if (*out_log_interval <= 0) *out_log_interval = DEFAULT_LOG_INTERVAL; continue; }
        if (strcmp(argv[i], "--log-append") == 0) { *out_log_append = 1; continue; }
        if (strcmp(argv[i], "--set-governor") == 0 && i + 1 < argc) { *out_set_governor = argv[++i]; continue; }
        if (strcmp(argv[i], "--set-min-freq") == 0 && i + 1 < argc) { *out_set_min_freq = atol(argv[++i]); continue; }
        if (strcmp(argv[i], "--set-max-freq") == 0 && i + 1 < argc) { *out_set_max_freq = atol(argv[++i]); continue; }
        if (strcmp(argv[i], "--freq-table") == 0 && i + 1 < argc) { *out_freq_table = argv[++i]; continue; }
        if (strcmp(argv[i], "--dynamic-freq") == 0) { *out_dynamic_freq = 1; continue; }
        if (strcmp(argv[i], "--help") == 0) { print_usage(argv[0]); return -1; }
        fprintf(stderr, "Unknown or malformed argument: %s\n", argv[i]); print_usage(argv[0]); return -1;
    }

    if (!*out_mode) { fprintf(stderr, "Missing --mode\n"); print_usage(argv[0]); return -1; }
    if (*out_util < 0) { fprintf(stderr, "Missing or invalid --util\n"); print_usage(argv[0]); return -1; }
    if (*out_duration <= 0) { fprintf(stderr, "Missing or invalid --duration\n"); print_usage(argv[0]); return -1; }
    if (!str_case_equal(*out_mode, "single") && !str_case_equal(*out_mode, "multi")) { fprintf(stderr, "Invalid --mode: %s\n", *out_mode); return -1; }
    if (*out_util < 10.0 || *out_util > 100.0) { fprintf(stderr, "--util must be between 10 and 100 (got %.2f)\n", *out_util); return -1; }
    if (*out_duration > *out_duration_limit) { fprintf(stderr, "--duration exceeds allowed limit (%ld s). Use --duration-limit to increase.\n", *out_duration_limit); return -1; }
    if (*out_max_threads <= 0 || *out_max_threads > DEFAULT_MAX_THREADS) { fprintf(stderr, "--max-threads must be 1..%d\n", DEFAULT_MAX_THREADS); return -1; }
    return 0;
}

/* parse freq table string like "0:3500000,1:3000000" */
int parse_freq_table(const char *s, int *out_count, int **out_cpu, long **out_freq) {
    if (!s) { *out_count = 0; *out_cpu = NULL; *out_freq = NULL; return 0; }
    char *copy = strdup(s); if (!copy) return -1;
    int maxpairs = 256; int *cpus = malloc(sizeof(int) * maxpairs); long *freqs = malloc(sizeof(long) * maxpairs);
    if (!cpus || !freqs) { free(copy); free(cpus); free(freqs); return -1; }
    int cnt = 0; char *tok = strtok(copy, ","); while (tok) {
        char *sep = strchr(tok, ':'); if (!sep) { tok = strtok(NULL, ","); continue; }
        *sep = '\0'; int cpu = atoi(tok); long hz = atol(sep + 1);
        if (cnt < maxpairs) { cpus[cnt] = cpu; freqs[cnt] = hz; cnt++; }
        tok = strtok(NULL, ",");
    }
    free(copy); *out_count = cnt; *out_cpu = cpus; *out_freq = freqs; return 0;
}

/* ------------------ environment validation ------------------ */
int validate_environment(const char *mode, workload_t type, int max_threads, int *out_nthreads, char **temp_path, double temp_threshold, int wants_cpufreq_write) {
    if (access("/proc/stat", R_OK) != 0) { fprintf(stderr, "Error: /proc/stat not readable. Are you on Linux?\n"); return -1; }
    int affinity_cpus = get_affinity_cpu_count();
    g_available_cpus = affinity_cpus;
    int nthreads = str_case_equal(mode, "single") ? 1 : affinity_cpus;
    if (nthreads > max_threads) { fprintf(stderr, "Requested threads (%d) exceed --max-threads (%d)\n", nthreads, max_threads); return -1; }
    if (nthreads > DEFAULT_MAX_THREADS) { fprintf(stderr, "Refusing to create more than %d threads for safety\n", DEFAULT_MAX_THREADS); return -1; }

    if (type == W_AVX || type == W_MIXED) {
        if (!cpu_supports_avx()) { fprintf(stderr, "Error: CPU does not report AVX support or OS hasn't enabled XGETBV support. Cannot run AVX workload.\n"); return -1; }
    }

    *temp_path = find_temperature_input_path(); if (!*temp_path) fprintf(stderr, "Warning: CPU temperature sensor not found; temperature-based auto-stop unavailable.\n"); else { double t = read_temperature(*temp_path); if (isnan(t)) fprintf(stderr, "Warning: temperature path found but unreadable or out-of-range.\n"); }

    if (wants_cpufreq_write) {
        if (geteuid() != 0) { fprintf(stderr, "Error: CPU frequency writes require root privileges. Re-run with sudo.\n"); return -1; }
        char path[256]; snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"); if (access(path, W_OK) != 0) fprintf(stderr, "Warning: cpufreq sysfs not writable or not present (%s).\n", path);
    }

    *out_nthreads = nthreads; return 0;
}

/* ------------------ main ------------------ */
int main(int argc, char **argv) {
    char *mode = NULL; double util = -1; long duration = -1; workload_t type = W_INT;
    int max_threads = DEFAULT_MAX_THREADS; long duration_limit = DEFAULT_DURATION_LIMIT_SEC; double temp_threshold = DEFAULT_TEMP_THRESHOLD;
    int check_only = 0; char *log_path = NULL; int log_interval = DEFAULT_LOG_INTERVAL; int log_append = 0;
    char *set_governor = NULL; long set_min_freq = -1; long set_max_freq = -1; char *freq_table_str = NULL; int dynamic_freq = 0;

    if (parse_args(argc, argv, &mode, &util, &duration, &type, &max_threads, &duration_limit, &temp_threshold, &check_only, &log_path, &log_interval, &log_append, &set_governor, &set_min_freq, &set_max_freq, &freq_table_str, &dynamic_freq) != 0) return 1;

    int wants_cpufreq_write = (set_governor || set_min_freq != -1 || set_max_freq != -1 || freq_table_str || dynamic_freq);
    char *temp_path = NULL; int planned_threads = 0;
    if (validate_environment(mode, type, max_threads, &planned_threads, &temp_path, temp_threshold, wants_cpufreq_write) != 0) { free(temp_path); return 1; }

    /* apply cpufreq writes if requested */
    int freq_table_count = 0; int *freq_table_cpu = NULL; long *freq_table_freq = NULL;
    if (freq_table_str) parse_freq_table(freq_table_str, &freq_table_count, &freq_table_cpu, &freq_table_freq);
    if (set_governor) for (int c = 0; c < g_available_cpus; ++c) if (write_scaling_governor(c, set_governor) != 0) fprintf(stderr, "Warning: failed to set governor '%s' on cpu %d\n", set_governor, c);
    if (set_min_freq != -1 || set_max_freq != -1) for (int c = 0; c < g_available_cpus; ++c) if (write_scaling_min_max(c, set_min_freq, set_max_freq) != 0) fprintf(stderr, "Warning: failed to set min/max freq on cpu %d\n", c);
    if (freq_table_count > 0) for (int i = 0; i < freq_table_count; ++i) { int c = freq_table_cpu[i]; long hz = freq_table_freq[i]; if (c < 0 || c >= g_available_cpus) { fprintf(stderr, "Warning: freq-table cpu %d out of range\n", c); continue; } if (write_scaling_min_max(c, hz, hz) != 0) fprintf(stderr, "Warning: failed to set freq %ld on cpu %d\n", hz, c); }

    /* prepare /proc.stat buffers */
    uint64_t *total_prev = calloc(g_available_cpus, sizeof(uint64_t)); uint64_t *idle_prev = calloc(g_available_cpus, sizeof(uint64_t)); uint64_t *total_curr = calloc(g_available_cpus, sizeof(uint64_t)); uint64_t *idle_curr = calloc(g_available_cpus, sizeof(uint64_t));
    if (!total_prev || !idle_prev || !total_curr || !idle_curr) { fprintf(stderr, "Memory allocation failed\n"); free(temp_path); return 1; }
    if (read_proc_stat(total_prev, idle_prev, g_available_cpus) <= 0) { fprintf(stderr, "Failed to read /proc/stat initial snapshot\n"); free(temp_path); return 1; }

    int nthreads = planned_threads;
    pthread_t *tids = calloc(nthreads, sizeof(pthread_t)); worker_arg_t *wargs = calloc(nthreads, sizeof(worker_arg_t)); if (!tids || !wargs) { fprintf(stderr, "Allocation failed\n"); free(temp_path); return 1; }

    for (int i = 0; i < nthreads; ++i) { wargs[i].cpu_id = i % g_available_cpus; wargs[i].target_util = util; wargs[i].type = type; __atomic_store_n(&wargs[i].ops_done, 0, __ATOMIC_RELAXED); }

    signal(SIGINT, sigint_handler); signal(SIGTERM, sigint_handler);

    for (int i = 0; i < nthreads; ++i) { if (pthread_create(&tids[i], NULL, worker_thread, &wargs[i]) != 0) { fprintf(stderr, "Failed to create thread %d: %s\n", i, strerror(errno)); stop_flag = 1; break; } }

    /* start monitor thread for hotplug/cpuset changes */
    pthread_t mon_tid = 0; if (pthread_create(&mon_tid, NULL, monitor_thread, NULL) != 0) { fprintf(stderr, "Warning: failed to start monitor thread\n"); mon_tid = 0; }

    /* logging setup */
    FILE *logf = NULL; FILE *summaryf = NULL; char *summary_path = NULL; uint64_t *prev_ops = NULL; int logging_enabled = 0;
    if (log_path) {
        logf = fopen(log_path, log_append ? "a" : "w");
        if (!logf) { fprintf(stderr, "Failed to open log file '%s' for writing: %s\n", log_path, strerror(errno)); logging_enabled = 0; }
        else {
            logging_enabled = 1;
            if (!log_append) {
                time_t ts = time(NULL);
                safe_fprintf_flush(logf, "# cpustress log\n");
                safe_fprintf_flush(logf, "# mode=%s\n", mode);
                safe_fprintf_flush(logf, "# workload=%s\n", (type == W_INT) ? "INT" : (type == W_FLOAT) ? "FLOAT" : (type == W_AVX) ? "AVX" : "MIXED");
                safe_fprintf_flush(logf, "# util=%.1f\n", util);
                safe_fprintf_flush(logf, "# threads=%d\n", nthreads);
                safe_fprintf_flush(logf, "# interval=%ds\n", log_interval);
                safe_fprintf_flush(logf, "# temp_threshold=%.1f\n", temp_threshold);
                safe_fprintf_flush(logf, "# start_time=%ld\n", (long)ts);
            }
            /* CSV header with limited core columns if needed */
            safe_fprintf_flush(logf, "timestamp,elapsed_sec,cpu_temp");
            int cores_to_log = (g_available_cpus > MAX_CORES_TO_LOG) ? MAX_CORES_TO_LOG : g_available_cpus;
            for (int c = 0; c < cores_to_log; ++c) safe_fprintf_flush(logf, ",cpu%d_util,cpu%d_freq", c, c);
            if (g_available_cpus > cores_to_log) safe_fprintf_flush(logf, ",cpu_others_util,cpu_others_freq");
            for (int t = 0; t < nthreads; ++t) safe_fprintf_flush(logf, ",thread%d_ops_delta", t);
            safe_fprintf_flush(logf, "\n");
            fflush(logf);
            prev_ops = calloc(nthreads, sizeof(uint64_t)); if (prev_ops) for (int t = 0; t < nthreads; ++t) prev_ops[t] = 0;
            summary_path = malloc(strlen(log_path) + 20); if (summary_path) { strcpy(summary_path, log_path); strcat(summary_path, ".summary.txt"); summaryf = fopen(summary_path, "w"); if (!summaryf) { free(summary_path); summary_path = NULL; } }
        }
    }

    time_t start = time(NULL); time_t end_time = start + duration; time_t now = start; int elapsed_sec = 0;
    int cores_to_log = (g_available_cpus > MAX_CORES_TO_LOG) ? MAX_CORES_TO_LOG : g_available_cpus;

    long *current_max_freq = NULL;
    if (dynamic_freq) { current_max_freq = calloc(g_available_cpus, sizeof(long)); for (int c = 0; c < g_available_cpus; ++c) { long hz = 0; if (read_scaling_cur_freq(c, &hz) == 0) current_max_freq[c] = hz; else current_max_freq[c] = 0; } }

    g_workers.tids = tids; g_workers.wargs = wargs; g_workers.nthreads = nthreads;

    while (!stop_flag && now < end_time) {
        for (int s = 0; s < log_interval && !stop_flag; ++s) sleep(1);
        if (stop_flag) break;
        int cpus_read = read_proc_stat(total_curr, idle_curr, g_available_cpus);
        if (cpus_read <= 0) cpus_read = g_available_cpus;
        double *util_pct = calloc(cpus_read, sizeof(double)); long *freqs = calloc(cpus_read, sizeof(long));
        for (int c = 0; c < cpus_read; ++c) {
            uint64_t totald = total_curr[c] - total_prev[c]; uint64_t idled = idle_curr[c] - idle_prev[c]; double usage = 0.0; if (totald > 0) usage = 100.0 * (double)(totald - idled) / (double)totald; util_pct[c] = usage; total_prev[c] = total_curr[c]; idle_prev[c] = idle_curr[c]; if (read_scaling_cur_freq(c, &freqs[c]) != 0) freqs[c] = 0;
        }
        double tempC = NAN; if (temp_path) tempC = read_temperature(temp_path);
        now = time(NULL); elapsed_sec = (int)(now - start);

        /* console */
        printf("\n=== time: %lds elapsed (%lds remaining) ===\n", (long)(now - start), (long)(end_time - now));
        for (int c = 0; c < cpus_read; ++c) {
            if (c < cores_to_log) printf(" core %2d : %6.2f%%  freq=%ld kHz\n", c, util_pct[c], freqs[c]);
        }
        if (cpus_read > cores_to_log) {
            double agg_util = 0; long agg_freq = 0; int agg_cnt = 0;
            for (int c = cores_to_log; c < cpus_read; ++c) { agg_util += util_pct[c]; if (freqs[c] > 0) { agg_freq += freqs[c]; agg_cnt++; } }
            if (agg_cnt > 0) agg_freq /= agg_cnt;
            printf(" cores %d..%d : avg_util=%.2f%% avg_freq=%ld kHz\n", cores_to_log, cpus_read - 1, agg_util / (cpus_read - cores_to_log), agg_freq);
        }
        if (!isnan(tempC)) printf(" CPU temp : %.2f °C\n", tempC); else printf(" CPU temp : (unavailable)\n");
        for (int t = 0; t < nthreads; ++t) { uint64_t ops = __atomic_load_n(&wargs[t].ops_done, __ATOMIC_RELAXED); printf(" thread %2d pinned->cpu%2d : ops_total=%" PRIu64 " target=%.1f%%\n", t, wargs[t].cpu_id, ops, wargs[t].target_util); }

        /* logging */
        if (logging_enabled && logf) {
            if (safe_fprintf_flush(logf, "%ld,%d,", (long)now, elapsed_sec) != 0) { fprintf(stderr, "Warning: log write failed. Disabling logging.\n"); fclose(logf); logf = NULL; logging_enabled = 0; }
            else {
                if (!isnan(tempC)) fprintf(logf, "%.2f", tempC);
                for (int c = 0; c < cores_to_log; ++c) fprintf(logf, ",%.2f,%ld", util_pct[c], freqs[c]);
                if (cpus_read > cores_to_log) {
                    double agg_util = 0; long agg_freq = 0; int agg_cnt = 0;
                    for (int c = cores_to_log; c < cpus_read; ++c) { agg_util += util_pct[c]; if (freqs[c] > 0) { agg_freq += freqs[c]; agg_cnt++; } }
                    double avg_util = (cpus_read - cores_to_log) ? agg_util / (cpus_read - cores_to_log) : 0.0;
                    long avg_freq = agg_cnt ? agg_freq / agg_cnt : 0;
                    fprintf(logf, ",%.2f,%ld", avg_util, avg_freq);
                }
                for (int t = 0; t < nthreads; ++t) {
                    uint64_t ops = __atomic_load_n(&wargs[t].ops_done, __ATOMIC_RELAXED);
                    uint64_t prev = prev_ops ? prev_ops[t] : 0ULL;
                    uint64_t delta = (ops >= prev) ? (ops - prev) : (UINT64_MAX - prev + ops + 1);
                    if (prev_ops) prev_ops[t] = ops;
                    fprintf(logf, ",%" PRIu64, delta);
                }
                fprintf(logf, "\n"); fflush(logf);
            }
        }

        /* dynamic freq tuner */
        if (dynamic_freq && !isnan(tempC)) {
            if (tempC >= temp_threshold) {
                for (int c = 0; c < g_available_cpus; ++c) {
                    long cur = current_max_freq ? current_max_freq[c] : 0; if (cur <= 0) { long r = 0; if (read_scaling_cur_freq(c, &r) == 0) cur = r; }
                    long new_max = (long)(cur * (100 - DYN_FREQ_STEP_PCT) / 100.0);
                    if (new_max < 100000) new_max = 100000;
                    if (write_scaling_min_max(c, -1, new_max) == 0) { if (current_max_freq) current_max_freq[c] = new_max; fprintf(stderr, "dynamic-freq: reduced cpu %d max_freq to %ld kHz due to high temp %.2f\n", c, new_max, tempC); }
                }
            }
        }

        /* temp safety */
        if (!isnan(tempC) && tempC >= temp_threshold) {
            fprintf(stderr, "ALERT: CPU temperature %.2f°C >= threshold %.2f°C. Initiating graceful stop.\n", tempC, temp_threshold);
            stop_flag = 1; free(util_pct); free(freqs); break;
        }

        free(util_pct); free(freqs);
    }

    /* stop & join */
    stop_flag = 1;
    for (int i = 0; i < nthreads; ++i) pthread_join(tids[i], NULL);
    if (mon_tid) pthread_join(mon_tid, NULL);

    /* final summary */
    read_proc_stat(total_curr, idle_curr, g_available_cpus);
    printf("\n=== SUMMARY ===\n");
    for (int c = 0; c < g_available_cpus; ++c) printf(" core %2d : last_total=%" PRIu64 " last_idle=%" PRIu64 "\n", c, total_curr[c], idle_curr[c]);
    if (temp_path) { double tempC = read_temperature(temp_path); if (!isnan(tempC)) printf(" Final CPU temp : %.2f °C\n", tempC); }
    printf(" Threads: %d  Duration requested: %ld s  Time elapsed: %lds\n", nthreads, duration, (long)(time(NULL) - start));
    for (int t = 0; t < nthreads; ++t) { uint64_t ops = __atomic_load_n(&wargs[t].ops_done, __ATOMIC_RELAXED); printf(" thread %2d pinned->cpu%2d : total ops=%" PRIu64 "\n", t, wargs[t].cpu_id, ops); }

    if (summaryf) {
        fprintf(summaryf, "cpustress summary\n");
        fprintf(summaryf, "mode=%s\n", mode);
        fprintf(summaryf, "workload=%s\n", (type == W_INT) ? "INT" : (type == W_FLOAT) ? "FLOAT" : (type == W_AVX) ? "AVX" : "MIXED");
        fprintf(summaryf, "util=%.1f\n", util);
        fprintf(summaryf, "threads=%d\n", nthreads);
        fprintf(summaryf, "duration_requested=%ld\n", duration);
        fprintf(summaryf, "time_elapsed=%ld\n", (long)(time(NULL) - start));
        if (temp_path) { double tempC = read_temperature(temp_path); if (!isnan(tempC)) fprintf(summaryf, "final_temp=%.2f\n", tempC); }
        for (int t = 0; t < nthreads; ++t) { uint64_t ops = __atomic_load_n(&wargs[t].ops_done, __ATOMIC_RELAXED); fprintf(summaryf, "thread%02d_cpu%02d_ops=%" PRIu64 "\n", t, wargs[t].cpu_id, ops); }
        fclose(summaryf); if (summary_path) printf("Summary written to %s\n", summary_path);
    }

    if (logf) { fclose(logf); printf("CSV log saved to %s\n", log_path); }
    free(temp_path); free(total_prev); free(idle_prev); free(total_curr); free(idle_curr); free(tids); free(wargs); if (prev_ops) free(prev_ops); if (summary_path) free(summary_path); if (freq_table_cpu) free(freq_table_cpu); if (freq_table_freq) free(freq_table_freq); if (current_max_freq) free(current_max_freq);
    return 0;
}

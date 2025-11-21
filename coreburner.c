/*
 * cpustress.c  -- CPU stress test with enterprise-grade validation + logging
 *
 * Features added on top of previous version:
 * - CSV logging of per-interval metrics (--log path.csv)
 * - Human-readable summary file written at the end (path.csv.summary.txt)
 * - --log-interval N  (seconds, default 1)
 * - --log-append flag to append instead of overwrite
 * - CSV contains: timestamp, elapsed_sec, cpu_temp, per-core utils..., per-thread ops_delta
 * - Header block added to CSV as commented metadata
 * - Uses atomic reads for per-thread op counters to be race-safe
 *
 * Build:
 *   gcc -O2 -march=native -pthread -std=c11 -Wall -Wextra -o cpustress cpustress.c -lm
 *
 * Example:
 *   ./cpustress --mode multi --util 75 --duration 2m --type AVX --log run.csv --log-interval 1
 *
 *
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

#define CONTROL_PERIOD_MS 100  // busy/sleep control period
#define REPORT_INTERVAL_SEC 1  // default seconds between reports
#define DEFAULT_MAX_THREADS 256
#define DEFAULT_DURATION_LIMIT_SEC (24*3600) // 24 hours
#define DEFAULT_TEMP_THRESHOLD 90.0 // Celsius

static volatile sig_atomic_t stop_flag = 0;
static int available_cpus = 0;

void sigint_handler(int s) { (void)s; stop_flag = 1; }

/* ---------------------- helper: parse duration ---------------------- */
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

/* ---------------------- read /proc/stat for per-core usage ---------------------- */
int read_proc_stat(uint64_t *total_out, uint64_t *idle_out, int max_cpus) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) {
        return -1;
    }
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

/* ---------------------- temperature helper ---------------------- */
char *find_temperature_input_path() {
    const char *cands[] = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/thermal/thermal_zone1/temp",
        NULL
    };
    for (int i = 0; cands[i]; ++i) {
        if (access(cands[i], R_OK) == 0) return strdup(cands[i]);
    }
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
    if (v > 1000) return v / 1000.0;
    return (double)v;
}

/* ---------------------- workload kernels ---------------------- */
typedef enum { W_INT, W_FLOAT, W_AVX, W_MIXED } workload_t;

void int_work_unit(volatile uint64_t *state) {
    uint64_t x = *state;
    for (int i = 0; i < 256; ++i) {
        x += (x << 1) ^ 0x9e3779b97f4a7c15ULL;
        x ^= (x >> 7);
        x += 0x9e3779b97f4a7c15ULL;
    }
    *state = x;
}

void float_work_unit(volatile double *state) {
    double x = *state;
    for (int i = 0; i < 256; ++i) {
        x = x * 1.0000001 + 0.10000001;
        x = fmod(x, 100000.0);
    }
    *state = x;
}

void avx_work_unit(float *fbuf) {
    __m256 a = _mm256_loadu_ps(fbuf);
    __m256 b = _mm256_set1_ps(1.000001f);
    for (int i = 0; i < 256; ++i) {
        a = _mm256_add_ps(a, b);
        a = _mm256_mul_ps(a, _mm256_set1_ps(0.999999f));
    }
    _mm256_storeu_ps(fbuf, a);
}

/* ---------------------- CPU feature detection (AVX) ---------------------- */
static int check_xgetbv_avx() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int a, d;
    __asm__ volatile ( "xgetbv" : "=a"(a), "=d"(d) : "c"(0) );
    uint64_t xcr0 = ((uint64_t)d << 32) | a;
    return ((xcr0 & 0x6ULL) == 0x6ULL);
#else
    return 0;
#endif
}

int cpu_supports_avx() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return 0;
    int has_avx = (ecx >> 28) & 1;
    int has_osxsave = (ecx >> 27) & 1;
    if (!has_avx || !has_osxsave) return 0;
    if (!check_xgetbv_avx()) return 0;
    return 1;
#else
    return 0;
#endif
}

/* ---------------------- Worker thread ---------------------- */
typedef struct {
    int cpu_id;
    double target_util;
    workload_t type;
    uint64_t ops_done;
} worker_arg_t;

void *worker_thread(void *arg) {
    worker_arg_t *w = (worker_arg_t *)arg;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(w->cpu_id, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        fprintf(stderr, "Warning: cannot set affinity for thread -> cpu %d: %s\n", w->cpu_id, strerror(errno));
    }

    volatile uint64_t int_state = (uint64_t)(uintptr_t)w ^ 0xabcdef;
    volatile double float_state = (double)(w->cpu_id + 1) * 1.234567;
    float avx_buf[8] __attribute__((aligned(32)));
    for (int i = 0; i < 8; ++i) avx_buf[i] = (float)(i + w->cpu_id);

    const long period_ns = CONTROL_PERIOD_MS * 1000000L;
    double util = w->target_util;
    if (util < 0) util = 0;
    if (util > 100) util = 100;
    long busy_ns = (long)round((util / 100.0) * period_ns);
    long sleep_ns = period_ns - busy_ns;

    struct timespec t0, t1, ts_sleep;
    while (!stop_flag) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (busy_ns > 0) {
            if (w->type == W_INT) {
                for (;;) {
                    int_work_unit(&int_state);
                    __atomic_add_fetch(&w->ops_done, 1, __ATOMIC_RELAXED);
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    long elapsed = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
                    if (elapsed >= busy_ns || stop_flag) break;
                }
            } else if (w->type == W_FLOAT) {
                for (;;) {
                    float_work_unit(&float_state);
                    __atomic_add_fetch(&w->ops_done, 1, __ATOMIC_RELAXED);
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    long elapsed = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
                    if (elapsed >= busy_ns || stop_flag) break;
                }
            } else if (w->type == W_AVX) {
                for (;;) {
                    avx_work_unit(avx_buf);
                    __atomic_add_fetch(&w->ops_done, 1, __ATOMIC_RELAXED);
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    long elapsed = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
                    if (elapsed >= busy_ns || stop_flag) break;
                }
            } else {
                for (;;) {
                    int_work_unit(&int_state);
                    float_work_unit(&float_state);
                    avx_work_unit(avx_buf);
                    __atomic_add_fetch(&w->ops_done, 1, __ATOMIC_RELAXED);
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                    long elapsed = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
                    if (elapsed >= busy_ns || stop_flag) break;
                }
            }
        }
        if (sleep_ns > 0 && !stop_flag) {
            ts_sleep.tv_sec = sleep_ns / 1000000000L;
            ts_sleep.tv_nsec = sleep_ns % 1000000000L;
            nanosleep(&ts_sleep, NULL);
        }
    }

    return NULL;
}

/* ---------------------- CLI & validation ---------------------- */
void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s --mode single|multi --util N(10-100) --duration X[s|m|h] --type INT|FLOAT|AVX|MIXED [options]\n"
            "Options:\n"
            "  --max-threads N         Max worker threads (default %d, hard cap %d)\n"
            "  --duration-limit X[s|m|h]  Upper allowed duration (default 24h)\n"
            "  --temp-threshold N      Auto-stop if CPU temp >= N (C). Default %.1f\n"
            "  --log FILE              Write CSV log to FILE\n"
            "  --log-interval N        Log/report interval seconds (default %d)\n"
            "  --log-append            Append to the log file instead of overwrite\n"
            "  --check                 Validate args and show planned run; do not execute workload\n"
            "  --help                  Show this help\n"
            "Examples:\n"
            "  %s --mode multi --util 75 --duration 2m --type AVX --log run.csv --log-interval 1\n",
            prog, DEFAULT_MAX_THREADS, DEFAULT_MAX_THREADS, DEFAULT_TEMP_THRESHOLD, REPORT_INTERVAL_SEC, prog);
}

int str_case_equal(const char *a, const char *b) {
    return strcasecmp(a, b) == 0;
}

workload_t parse_type(const char *s) {
    if (!s) return W_INT;
    if (str_case_equal(s, "INT")) return W_INT;
    if (str_case_equal(s, "FLOAT")) return W_FLOAT;
    if (str_case_equal(s, "AVX")) return W_AVX;
    return W_MIXED;
}

int parse_args(int argc, char **argv,
               char **out_mode, double *out_util, long *out_duration,
               workload_t *out_type,
               int *out_max_threads, long *out_duration_limit,
               double *out_temp_threshold, int *out_check,
               char **out_log_path, int *out_log_interval, int *out_log_append) {
    *out_mode = NULL; *out_util = -1; *out_duration = -1; *out_type = W_INT;
    *out_max_threads = DEFAULT_MAX_THREADS; *out_duration_limit = DEFAULT_DURATION_LIMIT_SEC;
    *out_temp_threshold = DEFAULT_TEMP_THRESHOLD; *out_check = 0;
    *out_log_path = NULL; *out_log_interval = REPORT_INTERVAL_SEC; *out_log_append = 0;

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
        if (strcmp(argv[i], "--log-interval") == 0 && i + 1 < argc) { *out_log_interval = atoi(argv[++i]); if (*out_log_interval <= 0) *out_log_interval = REPORT_INTERVAL_SEC; continue; }
        if (strcmp(argv[i], "--log-append") == 0) { *out_log_append = 1; continue; }
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

int validate_environment(const char *mode, workload_t type, int max_threads, int *out_nthreads, char **temp_path, double temp_threshold) {
    if (access("/proc/stat", R_OK) != 0) { fprintf(stderr, "Error: /proc/stat not readable. Are you on Linux?\n"); return -1; }
    available_cpus = get_nprocs(); if (available_cpus <= 0) available_cpus = 1;
    int nthreads = str_case_equal(mode, "single") ? 1 : available_cpus;
    if (nthreads > max_threads) { fprintf(stderr, "Requested threads (%d) exceed --max-threads (%d)\n", nthreads, max_threads); return -1; }
    if (nthreads > DEFAULT_MAX_THREADS) { fprintf(stderr, "Refusing to create more than %d threads for safety\n", DEFAULT_MAX_THREADS); return -1; }

    cpu_set_t orig;
    if (sched_getaffinity(0, sizeof(orig), &orig) != 0) {
        fprintf(stderr, "Warning: sched_getaffinity failed: %s\n", strerror(errno));
    } else {
        for (int c = 0; c < nthreads; ++c) {
            cpu_set_t test; CPU_ZERO(&test); CPU_SET(c % available_cpus, &test);
            if (sched_setaffinity(0, sizeof(test), &test) != 0) {
                fprintf(stderr, "Warning: cannot set process affinity to cpu %d: %s\n", c % available_cpus, strerror(errno));
            }
        }
        if (sched_setaffinity(0, sizeof(orig), &orig) != 0) { /* best-effort restore */ }
    }

    if (type == W_AVX || type == W_MIXED) {
        if (!cpu_supports_avx()) { fprintf(stderr, "Error: CPU does not report AVX support or OS hasn't enabled XGETBV support. Cannot run AVX workload.\n"); return -1; }
    }

    *temp_path = find_temperature_input_path();
    if (!*temp_path) {
        fprintf(stderr, "Warning: CPU temperature sensor not found; temperature-based auto-stop unavailable.\n");
    } else {
        double t = read_temperature(*temp_path);
        if (isnan(t)) fprintf(stderr, "Warning: temperature path found but unreadable.\n");
    }

    *out_nthreads = nthreads; return 0;
}

/* ---------------------- main ---------------------- */
int main(int argc, char **argv) {
    char *mode = NULL;
    double util = -1;
    long duration = -1;
    workload_t type = W_INT;
    int max_threads = DEFAULT_MAX_THREADS;
    long duration_limit = DEFAULT_DURATION_LIMIT_SEC;
    double temp_threshold = DEFAULT_TEMP_THRESHOLD;
    int check_only = 0;
    char *log_path = NULL;
    int log_interval = REPORT_INTERVAL_SEC;
    int log_append = 0;

    if (parse_args(argc, argv, &mode, &util, &duration, &type, &max_threads, &duration_limit, &temp_threshold, &check_only, &log_path, &log_interval, &log_append) != 0) {
        return 1;
    }

    char *temp_path = NULL;
    int planned_threads = 0;
    if (validate_environment(mode, type, max_threads, &planned_threads, &temp_path, temp_threshold) != 0) { free(temp_path); return 1; }

    if (check_only) {
        printf("Validation successful (dry-run). Planned configuration:\n");
        printf("  mode: %s\n", mode);
        printf("  threads: %d\n", planned_threads);
        printf("  util per-thread: %.1f%%\n", util);
        printf("  duration: %ld s\n", duration);
        const char *tname = (type == W_INT) ? "INT" : (type == W_FLOAT ? "FLOAT" : (type == W_AVX ? "AVX" : "MIXED"));
        printf("  workload type: %s\n", tname);
        if (temp_path) printf("  temperature sensor: %s\n", temp_path); else printf("  temperature sensor: (not found)\n");
        printf("  temp-stop-threshold: %.1f C\n", temp_threshold);
        printf("  max-threads limit: %d\n", max_threads);
        if (log_path) printf("  log: %s (interval %ds, append=%d)\n", log_path, log_interval, log_append);
        free(temp_path);
        return 0;
    }

    uint64_t *total_prev = calloc(available_cpus, sizeof(uint64_t));
    uint64_t *idle_prev  = calloc(available_cpus, sizeof(uint64_t));
    uint64_t *total_curr = calloc(available_cpus, sizeof(uint64_t));
    uint64_t *idle_curr  = calloc(available_cpus, sizeof(uint64_t));
    if (!total_prev || !idle_prev || !total_curr || !idle_curr) { fprintf(stderr, "Memory allocation failed\n"); free(temp_path); return 1; }
    int r = read_proc_stat(total_prev, idle_prev, available_cpus);
    if (r <= 0) { fprintf(stderr, "Failed to read /proc/stat initial snapshot\n"); free(temp_path); return 1; }

    int nthreads = planned_threads;
    pthread_t *tids = calloc(nthreads, sizeof(pthread_t));
    worker_arg_t *wargs = calloc(nthreads, sizeof(worker_arg_t));
    if (!tids || !wargs) { fprintf(stderr, "Allocation failed\n"); free(temp_path); return 1; }

    for (int i = 0; i < nthreads; ++i) {
        wargs[i].cpu_id = i % available_cpus;
        wargs[i].target_util = util;
        wargs[i].type = type;
        wargs[i].ops_done = 0;
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    for (int i = 0; i < nthreads; ++i) {
        if (pthread_create(&tids[i], NULL, worker_thread, &wargs[i]) != 0) {
            fprintf(stderr, "Failed to create thread %d: %s\n", i, strerror(errno)); stop_flag = 1; break; }
    }

    // logging setup
    FILE *logf = NULL;
    FILE *summaryf = NULL;
    char *summary_path = NULL;
    uint64_t *prev_ops = NULL;
    if (log_path) {
        logf = fopen(log_path, log_append ? "a" : "w");
        if (!logf) { fprintf(stderr, "Failed to open log file '%s' for writing: %s\n", log_path, strerror(errno)); }
        else {
            // write header metadata as comments if new file
            if (!log_append) {
                time_t ts = time(NULL);
                fprintf(logf, "# cpustress log\n");
                fprintf(logf, "# mode=%s\n", mode);
                fprintf(logf, "# workload=%s\n", (type==W_INT)?"INT":(type==W_FLOAT)?"FLOAT":(type==W_AVX)?"AVX":"MIXED");
                fprintf(logf, "# util=%.1f\n", util);
                fprintf(logf, "# threads=%d\n", nthreads);
                fprintf(logf, "# interval=%ds\n", log_interval);
                fprintf(logf, "# temp_threshold=%.1f\n", temp_threshold);
                fprintf(logf, "# start_time=%ld\n", (long)ts);
            }
            // CSV header
            fprintf(logf, "timestamp,elapsed_sec,cpu_temp");
            for (int c = 0; c < available_cpus; ++c) fprintf(logf, ",cpu%d_util", c);
            for (int t = 0; t < nthreads; ++t) fprintf(logf, ",thread%d_ops_delta", t);
            fprintf(logf, "\n");
            fflush(logf);

            // prepare prev_ops array
            prev_ops = calloc(nthreads, sizeof(uint64_t));
            if (!prev_ops) { fprintf(stderr, "Failed to alloc prev_ops\n"); }
            else {
                for (int t = 0; t < nthreads; ++t) prev_ops[t] = 0;
            }

            // prepare summary path
            size_t lp = strlen(log_path);
            summary_path = malloc(lp + 20);
            if (summary_path) {
                strcpy(summary_path, log_path);
                strcat(summary_path, ".summary.txt");
                summaryf = fopen(summary_path, "w");
                if (!summaryf) { free(summary_path); summary_path = NULL; }
            }
        }
    }

    time_t start = time(NULL);
    time_t end_time = start + duration;
    time_t now = start;
    int elapsed_sec = 0;

    // main loop with variable reporting/logging interval
    while (!stop_flag && now < end_time) {
        int sleep_steps = log_interval;
        for (int s = 0; s < sleep_steps && !stop_flag; ++s) {
            sleep(1);
        }
        if (stop_flag) break;

        int cpus_read = read_proc_stat(total_curr, idle_curr, available_cpus);
        if (cpus_read <= 0) cpus_read = available_cpus;
        double *util_pct = calloc(cpus_read, sizeof(double));
        for (int c = 0; c < cpus_read; ++c) {
            uint64_t totald = total_curr[c] - total_prev[c];
            uint64_t idled  = idle_curr[c] - idle_prev[c];
            double usage = 0.0;
            if (totald > 0) usage = 100.0 * (double)(totald - idled) / (double)totald;
            util_pct[c] = usage;
            total_prev[c] = total_curr[c]; idle_prev[c] = idle_curr[c];
        }

        double tempC = NAN; if (temp_path) tempC = read_temperature(temp_path);
        now = time(NULL); elapsed_sec = (int)(now - start);

        // print console report
        printf("\n=== time: %lds elapsed (%lds remaining) ===\n", (long)(now - start), (long)(end_time - now));
        for (int c = 0; c < cpus_read; ++c) printf(" core %2d : %6.2f%%\n", c, util_pct[c]);
        if (!isnan(tempC)) printf(" CPU temp : %.2f °C\n", tempC); else printf(" CPU temp : (unavailable)\n");
        for (int t = 0; t < nthreads; ++t) {
            uint64_t ops = __atomic_load_n(&wargs[t].ops_done, __ATOMIC_RELAXED);
            printf(" thread %2d pinned->cpu%2d : ops_total=%" PRIu64 " target=%.1f%%\n", t, wargs[t].cpu_id, ops, wargs[t].target_util);
        }

        // logging to CSV: timestamp,elapsed_sec,cpu_temp, per-core utils..., per-thread ops_delta
        if (logf) {
            fprintf(logf, "%ld,%d,", (long)now, elapsed_sec);
            if (!isnan(tempC)) fprintf(logf, "%.2f", tempC); else fprintf(logf, "");
            for (int c = 0; c < cpus_read; ++c) fprintf(logf, ",%.2f", util_pct[c]);

            // per-thread ops delta since last log
            for (int t = 0; t < nthreads; ++t) {
                uint64_t ops = __atomic_load_n(&wargs[t].ops_done, __ATOMIC_RELAXED);
                uint64_t delta = ops - (prev_ops ? prev_ops[t] : 0ULL);
                if (prev_ops) prev_ops[t] = ops;
                fprintf(logf, ",%" PRIu64, delta);
            }
            fprintf(logf, "\n");
            fflush(logf);
        }

        // temperature safety
        if (!isnan(tempC) && tempC >= temp_threshold) {
            fprintf(stderr, "ALERT: CPU temperature %.2f°C >= threshold %.2f°C. Initiating graceful stop.\n", tempC, temp_threshold);
            stop_flag = 1; free(util_pct); break;
        }

        free(util_pct);
    }

    // stop & join
    stop_flag = 1;
    for (int i = 0; i < nthreads; ++i) pthread_join(tids[i], NULL);

    // final summary: write to console and to summary file if requested
    read_proc_stat(total_curr, idle_curr, available_cpus);
    printf("\n=== SUMMARY ===\n");
    for (int c = 0; c < available_cpus; ++c) printf(" core %2d : last_total=%" PRIu64 " last_idle=%" PRIu64 "\n", c, total_curr[c], idle_curr[c]);
    if (temp_path) {
        double tempC = read_temperature(temp_path);
        if (!isnan(tempC)) printf(" Final CPU temp : %.2f °C\n", tempC);
    }
    printf(" Threads: %d  Duration requested: %ld s  Time elapsed: %lds\n", nthreads, duration, (long)(time(NULL) - start));
    for (int t = 0; t < nthreads; ++t) {
        uint64_t ops = __atomic_load_n(&wargs[t].ops_done, __ATOMIC_RELAXED);
        printf(" thread %2d pinned->cpu%2d : total ops=%" PRIu64 "\n", t, wargs[t].cpu_id, ops);
    }

    if (summaryf) {
        fprintf(summaryf, "cpustress summary\n");
        fprintf(summaryf, "mode=%s\n", mode);
        fprintf(summaryf, "workload=%s\n", (type==W_INT)?"INT":(type==W_FLOAT)?"FLOAT":(type==W_AVX)?"AVX":"MIXED");
        fprintf(summaryf, "util=%.1f\n", util);
        fprintf(summaryf, "threads=%d\n", nthreads);
        fprintf(summaryf, "duration_requested=%ld\n", duration);
        fprintf(summaryf, "time_elapsed=%ld\n", (long)(time(NULL) - start));
        if (temp_path) {
            double tempC = read_temperature(temp_path);
            if (!isnan(tempC)) fprintf(summaryf, "final_temp=%.2f\n", tempC);
        }
        for (int t = 0; t < nthreads; ++t) {
            uint64_t ops = __atomic_load_n(&wargs[t].ops_done, __ATOMIC_RELAXED);
            fprintf(summaryf, "thread%02d_cpu%02d_ops=%" PRIu64 "\n", t, wargs[t].cpu_id, ops);
        }
        fclose(summaryf);
        printf("Summary written to %s\n", summary_path);
    }

    if (logf) { fclose(logf); printf("CSV log saved to %s\n", log_path); }

    free(temp_path);
    free(total_prev); free(idle_prev); free(total_curr); free(idle_curr);
    free(tids); free(wargs); if (prev_ops) free(prev_ops); if (summary_path) free(summary_path);
    return 0;
}

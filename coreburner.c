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

 /*******************************************************
 * CoreBurner — Advanced CPU Stress & Telemetry Tool
 * CHUNK 1 / 5
 *  - Headers
 *  - Global constants
 *  - Utility helpers
 *  - Temperature system
 *  - CPUFreq helpers
 *  - Workload units (INT / FLOAT / AVX)
 *******************************************************/

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
#define DEFAULT_DURATION_LIMIT_SEC (24 * 3600)
#define DEFAULT_TEMP_THRESHOLD 90.0
#define DYN_FREQ_STEP_PCT 10
#define MAX_CORES_TO_LOG 64

#define TEMP_SANITY_MIN -20.0
#define TEMP_SANITY_MAX 150.0

static volatile sig_atomic_t stop_flag = 0;
static pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;

void sigint_handler(int s) { (void)s; stop_flag = 1; }

/***********************************************************
 *                   CPU Affinity Helpers
 ***********************************************************/
static int get_affinity_cpu_count() {
    cpu_set_t set;
    CPU_ZERO(&set);

    if (sched_getaffinity(0, sizeof(set), &set) != 0)
        return get_nprocs();

    int cnt = 0;
    for (int i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &set))
            cnt++;

    return cnt > 0 ? cnt : 1;
}

/***********************************************************
 *                     Duration Parsing
 ***********************************************************/
long parse_duration_seconds(const char *s) {
    if (!s) return -1;

    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);

    if (end == s || errno != 0)
        return -1;

    while (*end == ' ') ++end;

    if (*end == '\0' || *end == 's' || *end == 'S')
        return (long)round(v);

    if (*end == 'm' || *end == 'M')
        return (long)round(v * 60.0);

    if (*end == 'h' || *end == 'H')
        return (long)round(v * 3600.0);

    return -1;
}

/***********************************************************
 *                      /proc/stat Parsing
 ***********************************************************/
int read_proc_stat(uint64_t *total_out, uint64_t *idle_out, int max_cpus) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;

    char line[512];
    int idx = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0)
            break;

        if (line[3] == ' ')
            continue;  // skip aggregate

        unsigned long long user=0, nice=0, system=0, idle=0;
        unsigned long long iowait=0, irq=0, softirq=0, steal=0;

        int matched = sscanf(
            line,
            "cpu%*d %llu %llu %llu %llu %llu %llu %llu %llu",
            &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal
        );

        if (matched < 4)
            continue;

        uint64_t idle_all = idle + (matched >= 5 ? iowait : 0);

        uint64_t nonidle =
            user + nice + system +
            (matched >= 6 ? irq : 0) +
            (matched >= 7 ? softirq : 0) +
            (matched >= 8 ? steal : 0);

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

/***********************************************************
 *                Temperature Sensor Helpers
 ***********************************************************/
char *find_temperature_input_path() {
    const char *fixed[] = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/thermal/thermal_zone1/temp",
        NULL
    };

    for (int i = 0; fixed[i]; i++)
        if (access(fixed[i], R_OK) == 0)
            return strdup(fixed[i]);

    char buf[256];
    for (int i = 0; i < 64; ++i) {
        snprintf(buf, sizeof(buf), "/sys/class/hwmon/hwmon%d/temp1_input", i);
        if (access(buf, R_OK) == 0)
            return strdup(buf);
    }

    return NULL;
}

double read_temperature(const char *path) {
    if (!path) return NAN;

    FILE *f = fopen(path, "r");
    if (!f) return NAN;

    long raw = 0;
    if (fscanf(f, "%ld", &raw) != 1) {
        fclose(f);
        return NAN;
    }
    fclose(f);

    double t = raw > 1000 ? raw / 1000.0 : (double)raw;

    if (t < TEMP_SANITY_MIN || t > TEMP_SANITY_MAX)
        return NAN;

    return t;
}

/***********************************************************
 *                  CPU Frequency Helpers
 ***********************************************************/
int read_scaling_cur_freq(int cpu, long *out_hz) {
    char path[256];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    long v = 0;
    if (fscanf(f, "%ld", &v) != 1) {
        fclose(f);
        return -1;
    }

    fclose(f);
    *out_hz = v;
    return 0;
}

int write_sysfs_int(const char *path, long value) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    int rc = fprintf(f, "%ld\n", value) < 0 ? -1 : 0;

    fclose(f);
    return rc;
}

int write_scaling_governor(int cpu, const char *gov) {
    char path[256];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    int rc = fprintf(f, "%s\n", gov) < 0 ? -1 : 0;

    fclose(f);
    return rc;
}

int write_scaling_min_max(int cpu, long min_hz, long max_hz) {
    char pmin[256], pmax[256];

    snprintf(pmin, sizeof(pmin),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", cpu);
    snprintf(pmax, sizeof(pmax),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", cpu);

    if (min_hz >= 0)
        if (write_sysfs_int(pmin, min_hz) != 0)
            return -1;

    if (max_hz >= 0)
        if (write_sysfs_int(pmax, max_hz) != 0)
            return -1;

    return 0;
}

/***********************************************************
 *                      Work Units
 ***********************************************************/
typedef enum { 
    W_INT, 
    W_FLOAT, 
    W_SSE, 
    W_AVX, 
    W_AVX2, 
    W_AVX512, 
    W_MIXED,
    W_AUTO 
} workload_t;

/* INT workload */
void int_work_unit(volatile uint64_t *state) {
    uint64_t x = *state;

    for (int i = 0; i < 256; ++i) {
        x += (x << 1) ^ 0x9e3779b97f4a7c15ULL;
        x ^= (x >> 7);
    }

    *state = x;
}

/* FLOAT workload */
void float_work_unit(volatile double *state) {
    double x = *state;

    for (int i = 0; i < 256; ++i) {
        x = x * 1.0000001 + 0.10000001;
        x = fmod(x, 100000.0);
    }

    *state = x;
}

/* SSE workload - 128-bit SIMD */
void sse_work_unit(float *buf) {
    __m128 a = _mm_loadu_ps(buf);
    __m128 b = _mm_set1_ps(1.000001f);

    for (int i = 0; i < 256; ++i) {
        a = _mm_add_ps(a, b);
        a = _mm_mul_ps(a, _mm_set1_ps(0.999999f));
    }

    _mm_storeu_ps(buf, a);
}

/* AVX workload - 256-bit FP only */
void avx_work_unit(float *buf) {
    __m256 a = _mm256_loadu_ps(buf);
    __m256 b = _mm256_set1_ps(1.000001f);

    for (int i = 0; i < 256; ++i) {
        a = _mm256_add_ps(a, b);
        a = _mm256_mul_ps(a, _mm256_set1_ps(0.999999f));
    }

    _mm256_storeu_ps(buf, a);
}

/* AVX2 workload - 256-bit with FMA */
void avx2_work_unit(float *buf) {
    __m256 a = _mm256_loadu_ps(buf);
    __m256 b = _mm256_set1_ps(1.000001f);
    __m256 c = _mm256_set1_ps(0.999999f);

    for (int i = 0; i < 256; ++i) {
        /* FMA: a = a * c + b */
        a = _mm256_fmadd_ps(a, c, b);
    }

    _mm256_storeu_ps(buf, a);
}

/* AVX-512 workload - 512-bit vectors */
void avx512_work_unit(float *buf) {
#ifdef __AVX512F__
    __m512 a = _mm512_loadu_ps(buf);
    __m512 b = _mm512_set1_ps(1.000001f);
    __m512 c = _mm512_set1_ps(0.999999f);

    for (int i = 0; i < 256; ++i) {
        /* FMA: a = a * c + b */
        a = _mm512_fmadd_ps(a, c, b);
    }

    _mm512_storeu_ps(buf, a);
#else
    /* Fallback to AVX2 if AVX-512 not available at compile time */
    avx2_work_unit(buf);
#endif
}

/*******************************************************
 * CoreBurner — CHUNK 2 / 5
 *  - AVX capability detection
 *  - Worker thread implementation
 *  - Monitor thread (CPU hotplug / cpuset tracking)
 *  - Logging helpers
 *******************************************************/

/*******************************************************
 *                  AVX Support Detection
 *******************************************************/
static int check_xgetbv_avx() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int a, d;
    __asm__ volatile("xgetbv" : "=a"(a), "=d"(d) : "c"(0));

    uint64_t xcr0 = ((uint64_t)d << 32) | a;

    /* Must have XMM + YMM state enabled */
    return ((xcr0 & 0x6ULL) == 0x6ULL);
#else
    return 0;
#endif
}

int cpu_supports_sse() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int eax, ebx, ecx, edx;

    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        return 0;

    /* Check for SSE4.2 (bit 20 of ECX) */
    return (ecx >> 20) & 1;
#else
    return 0;
#endif
}

int cpu_supports_avx() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int eax, ebx, ecx, edx;

    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        return 0;

    int has_avx     = (ecx >> 28) & 1;
    int has_osxsave = (ecx >> 27) & 1;

    if (!has_avx || !has_osxsave)
        return 0;

    if (!check_xgetbv_avx())
        return 0;

    return 1;
#else
    return 0;
#endif
}

int cpu_supports_avx2() {
#if defined(__x86_64__) || defined(__i386__)
    if (!cpu_supports_avx())
        return 0;

    unsigned int eax, ebx, ecx, edx;

    /* Check CPUID leaf 7, subleaf 0 */
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx))
        return 0;

    /* AVX2 is bit 5 of EBX */
    int has_avx2 = (ebx >> 5) & 1;
    /* FMA is bit 12 of ECX from leaf 1 */
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        return 0;
    int has_fma = (ecx >> 12) & 1;

    return has_avx2 && has_fma;
#else
    return 0;
#endif
}

int cpu_supports_avx512() {
#if defined(__x86_64__) || defined(__i386__)
    if (!cpu_supports_avx())
        return 0;

    unsigned int eax, ebx, ecx, edx;

    /* Check CPUID leaf 7, subleaf 0 */
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx))
        return 0;

    /* AVX-512F (Foundation) is bit 16 of EBX */
    int has_avx512f = (ebx >> 16) & 1;

    if (!has_avx512f)
        return 0;

    /* Check XCR0 for ZMM state (bits 5-7) */
    unsigned int xcr0_low, xcr0_high;
    __asm__ volatile("xgetbv" : "=a"(xcr0_low), "=d"(xcr0_high) : "c"(0));
    uint64_t xcr0 = ((uint64_t)xcr0_high << 32) | xcr0_low;

    /* Bits 5, 6, 7 must be set for ZMM state */
    return ((xcr0 & 0xE6ULL) == 0xE6ULL);
#else
    return 0;
#endif
}

/*******************************************************
 *                    Worker Thread
 *******************************************************/
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
    struct timespec req = {sec, nsec};
    struct timespec rem;

    while (!stop_flag) {
        if (nanosleep(&req, &rem) == 0)
            break;
        if (errno == EINTR) {
            req = rem;
            continue;
        }
        break;
    }
}

/*******************************************************
 *                    Monitor Thread
 * Detects CPU hotplug / cpuset changes
 *******************************************************/
void *monitor_thread(void *arg) {
    (void)arg;

    int prev_affinity = get_affinity_cpu_count();

    while (!stop_flag) {
        sleep(1);

        int current_affinity = get_affinity_cpu_count();

        if (current_affinity != prev_affinity) {
            pthread_mutex_lock(&global_lock);

            g_available_cpus = current_affinity;

            for (int i = 0; i < g_workers.nthreads; ++i) {
                int desired = g_workers.wargs[i].cpu_id;

                if (desired >= g_available_cpus) {
                    int newcpu = i % g_available_cpus;

                    g_workers.wargs[i].cpu_id = newcpu;

                    cpu_set_t cpuset;
                    CPU_ZERO(&cpuset);
                    CPU_SET(newcpu, &cpuset);

                    pthread_setaffinity_np(
                        g_workers.tids[i],
                        sizeof(cpu_set_t),
                        &cpuset
                    );
                }
            }

            pthread_mutex_unlock(&global_lock);
            prev_affinity = current_affinity;
        }
    }

    return NULL;
}

/*******************************************************
 *                   Logging Helper
 *******************************************************/
int safe_fprintf_flush(FILE *f, const char *fmt, ...) {
    if (!f) return -1;

    va_list ap;
    va_start(ap, fmt);

    int rc = vfprintf(f, fmt, ap);

    va_end(ap);

    if (rc < 0)
        return -1;

    if (fflush(f) != 0)
        return -1;

    return 0;
}


/*******************************************************
 * CoreBurner — CHUNK 3 / 5
 *  - CLI parsing
 *  - Workload type parser
 *  - Frequency table parsing
 *  - Mixed-ratio support
 *  - validate_environment()
 *******************************************************/

/***********************************************************
 *                      CLI Helpers
 ***********************************************************/
void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --mode single|multi|single-core-multi --util N(10-100) "
        "--duration X[s|m|h] --type AUTO|INT|FLOAT|SSE|AVX|AVX2|AVX512|MIXED [options]\n"
        "\n"
        "Modes:\n"
        "  single              Single thread on one core\n"
        "  multi               One thread per available core\n"
        "  single-core-multi   Multiple threads on a single core\n"
        "\n"
        "Options:\n"
        "  --max-threads N          Max worker threads (default %d)\n"
        "  --duration-limit X       Upper allowed duration (default 24h)\n"
        "  --temp-threshold N       Auto-stop if CPU temp >= N °C (default %.1f)\n"
        "  --log FILE               Write CSV log to FILE\n"
        "  --log-interval N         Log/report interval (default %d sec)\n"
        "  --log-append             Append instead of overwrite\n"
        "\n"
        "Single-Core Multi-Thread Options:\n"
        "  --single-core-id N       CPU core ID to pin threads (default 0)\n"
        "  --single-core-threads N  Number of threads on single core (default 2)\n"
        "\n"
        "CPU Frequency / Governor (requires root):\n"
        "  --set-governor GOV       Set CPU governor\n"
        "  --set-min-freq HZ        Set scaling_min_freq\n"
        "  --set-max-freq HZ        Set scaling_max_freq\n"
        "  --freq-table LIST        Format: \"0:3200000,1:2800000,...\"\n"
        "\n"
        "Dynamic Frequency Management:\n"
        "  --dynamic-freq           Auto reduce freq when temp rises\n"
        "\n"
        "Mixed Workload Options:\n"
        "  --mixed-ratio A:B:C      INT:FLOAT:AVX ratios\n"
        "                           Example: --mixed-ratio 5:2:3\n"
        "\n"
        "Misc:\n"
        "  --check                  Validate config but do not run workload\n"
        "  --help                   Show this help\n",
        prog, DEFAULT_MAX_THREADS, DEFAULT_TEMP_THRESHOLD, DEFAULT_LOG_INTERVAL
    );
}

int str_case_equal(const char *a, const char *b) {
    return strcasecmp(a, b) == 0;
}

workload_t parse_type(const char *s) {
    if (!s) return W_AUTO;

    if (str_case_equal(s, "INT"))    return W_INT;
    if (str_case_equal(s, "FLOAT"))  return W_FLOAT;
    if (str_case_equal(s, "SSE"))    return W_SSE;
    if (str_case_equal(s, "AVX"))    return W_AVX;
    if (str_case_equal(s, "AVX2"))   return W_AVX2;
    if (str_case_equal(s, "AVX512")) return W_AVX512;
    if (str_case_equal(s, "MIXED"))  return W_MIXED;
    if (str_case_equal(s, "AUTO"))   return W_AUTO;
    return W_AUTO;
}

/***********************************************************
 *                 CLI Argument Parser
 ***********************************************************/
int parse_args(
    int argc, char **argv,
    char **out_mode, double *out_util, long *out_duration,
    workload_t *out_type,
    int *out_max_threads, long *out_duration_limit,
    double *out_temp_threshold, int *out_check,
    char **out_log_path, int *out_log_interval, int *out_log_append,
    char **out_set_governor, long *out_set_min_freq, long *out_set_max_freq,
    char **out_freq_table,
    int *out_dynamic_freq,
    char **out_mixed_ratio,
    int *out_single_core_id, int *out_single_core_threads)
{
    *out_mode = NULL;
    *out_util = -1;
    *out_duration = -1;
    *out_type = W_INT;

    *out_max_threads = DEFAULT_MAX_THREADS;
    *out_duration_limit = DEFAULT_DURATION_LIMIT_SEC;
    *out_temp_threshold = DEFAULT_TEMP_THRESHOLD;
    *out_check = 0;

    *out_log_path = NULL;
    *out_log_interval = DEFAULT_LOG_INTERVAL;
    *out_log_append = 0;

    *out_set_governor = NULL;
    *out_set_min_freq = -1;
    *out_set_max_freq = -1;
    *out_freq_table   = NULL;

    *out_dynamic_freq = 0;
    *out_mixed_ratio  = NULL;
    
    *out_single_core_id = 0;
    *out_single_core_threads = 2;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            *out_mode = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--util") == 0 && i + 1 < argc) {
            *out_util = atof(argv[++i]);
            continue;
        }

        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            *out_duration = parse_duration_seconds(argv[++i]);
            continue;
        }

        if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
            *out_type = parse_type(argv[++i]);
            continue;
        }

        if (strcmp(argv[i], "--max-threads") == 0 && i + 1 < argc) {
            *out_max_threads = atoi(argv[++i]);
            continue;
        }

        if (strcmp(argv[i], "--duration-limit") == 0 && i + 1 < argc) {
            *out_duration_limit = parse_duration_seconds(argv[++i]);
            continue;
        }

        if (strcmp(argv[i], "--temp-threshold") == 0 && i + 1 < argc) {
            *out_temp_threshold = atof(argv[++i]);
            continue;
        }

        if (strcmp(argv[i], "--check") == 0) {
            *out_check = 1;
            continue;
        }

        if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            *out_log_path = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--log-interval") == 0 && i + 1 < argc) {
            *out_log_interval = atoi(argv[++i]);
            if (*out_log_interval <= 0)
                *out_log_interval = DEFAULT_LOG_INTERVAL;
            continue;
        }

        if (strcmp(argv[i], "--log-append") == 0) {
            *out_log_append = 1;
            continue;
        }

        if (strcmp(argv[i], "--set-governor") == 0 && i + 1 < argc) {
            *out_set_governor = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--set-min-freq") == 0 && i + 1 < argc) {
            *out_set_min_freq = atol(argv[++i]);
            continue;
        }

        if (strcmp(argv[i], "--set-max-freq") == 0 && i + 1 < argc) {
            *out_set_max_freq = atol(argv[++i]);
            continue;
        }

        if (strcmp(argv[i], "--freq-table") == 0 && i + 1 < argc) {
            *out_freq_table = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--dynamic-freq") == 0) {
            *out_dynamic_freq = 1;
            continue;
        }

        if (strcmp(argv[i], "--mixed-ratio") == 0 && i + 1 < argc) {
            *out_mixed_ratio = argv[++i];
            continue;
        }
        
        if (strcmp(argv[i], "--single-core-id") == 0 && i + 1 < argc) {
            *out_single_core_id = atoi(argv[++i]);
            continue;
        }
        
        if (strcmp(argv[i], "--single-core-threads") == 0 && i + 1 < argc) {
            *out_single_core_threads = atoi(argv[++i]);
            continue;
        }

        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return -1;
        }

        fprintf(stderr, "Unknown or malformed argument: %s\n", argv[i]);
        print_usage(argv[0]);
        return -1;
    }

    /* Validate mandatory parameters */
    if (!*out_mode) {
        fprintf(stderr, "Missing --mode\n");
        return -1;
    }

    if (*out_util < 0) {
        fprintf(stderr, "Missing or invalid --util\n");
        return -1;
    }

    if (*out_duration <= 0) {
        fprintf(stderr, "Missing or invalid --duration\n");
        return -1;
    }

    if (*out_util < 10 || *out_util > 100) {
        fprintf(stderr, "--util must be between 10 and 100\n");
        return -1;
    }

    if (*out_max_threads <= 0 || *out_max_threads > DEFAULT_MAX_THREADS) {
        fprintf(stderr, "--max-threads must be between 1 and %d\n",
                DEFAULT_MAX_THREADS);
        return -1;
    }

    return 0;
}

/***********************************************************
 *                Frequency Table Parser
 ***********************************************************/
int parse_freq_table(
    const char *s,
    int *out_count,
    int **out_cpu,
    long **out_freq)
{
    if (!s) {
        *out_count = 0;
        *out_cpu = NULL;
        *out_freq = NULL;
        return 0;
    }

    char *copy = strdup(s);
    if (!copy) return -1;

    int maxpairs = 256;
    int *cpus  = malloc(sizeof(int) * maxpairs);
    long *freq = malloc(sizeof(long) * maxpairs);

    if (!cpus || !freq) {
        free(cpus);
        free(freq);
        free(copy);
        return -1;
    }

    int cnt = 0;

    char *tok = strtok(copy, ",");
    while (tok) {
        char *sep = strchr(tok, ':');
        if (!sep) {
            tok = strtok(NULL, ",");
            continue;
        }

        *sep = '\0';

        cpus[cnt]  = atoi(tok);
        freq[cnt]  = atol(sep + 1);

        cnt++;
        tok = strtok(NULL, ",");
    }

    free(copy);

    *out_cpu  = cpus;
    *out_freq = freq;
    *out_count = cnt;

    return 0;
}

/***********************************************************
 *             Mixed Ratio Parser (A:B:C)
 ***********************************************************/
typedef struct {
    int r_int;
    int r_float;
    int r_avx;
    int total;
} mixed_ratio_t;

static mixed_ratio_t g_mixed_ratio = {0,0,0,0};

int parse_mixed_ratio(const char *s, mixed_ratio_t *mr) {
    if (!s) return -1;

    int a=0, b=0, c=0;

    if (sscanf(s, "%d:%d:%d", &a, &b, &c) != 3)
        return -1;

    if (a < 0 || b < 0 || c < 0)
        return -1;

    int total = a + b + c;

    if (total <= 0)
        return -1;

    mr->r_int   = a;
    mr->r_float = b;
    mr->r_avx   = c;
    mr->total   = total;

    return 0;
}

/***********************************************************
 *             Auto-detect Best SIMD Level
 ***********************************************************/
workload_t auto_detect_best_simd() {
    if (cpu_supports_avx512()) {
        fprintf(stderr, "Auto-detected: AVX-512 (512-bit SIMD)\n");
        return W_AVX512;
    }
    if (cpu_supports_avx2()) {
        fprintf(stderr, "Auto-detected: AVX2 (256-bit with FMA)\n");
        return W_AVX2;
    }
    if (cpu_supports_avx()) {
        fprintf(stderr, "Auto-detected: AVX (256-bit FP)\n");
        return W_AVX;
    }
    if (cpu_supports_sse()) {
        fprintf(stderr, "Auto-detected: SSE (128-bit)\n");
        return W_SSE;
    }
    fprintf(stderr, "Auto-detected: INT (no SIMD)\n");
    return W_INT;
}

/***********************************************************
 *             Environment Validation
 ***********************************************************/
int validate_environment(
    const char *mode,
    workload_t type,
    int max_threads,
    int *out_nthreads,
    char **temp_path,
    double temp_threshold,
    int wants_cpufreq_write,
    const char *mixed_ratio_str,
    int single_core_id,
    int single_core_threads)
{
    if (access("/proc/stat", R_OK) != 0) {
        fprintf(stderr, "Error: /proc/stat not readable\n");
        return -1;
    }

    /* Determine worker thread count */
    int affinity = get_affinity_cpu_count();
    g_available_cpus = affinity;

    int nthreads;
    if (str_case_equal(mode, "single")) {
        nthreads = 1;
    } else if (str_case_equal(mode, "single-core-multi")) {
        /* Validate single-core-multi parameters */
        if (single_core_id < 0 || single_core_id >= affinity) {
            fprintf(stderr,
                    "Error: --single-core-id=%d is out of range (0-%d)\n",
                    single_core_id, affinity - 1);
            return -1;
        }
        if (single_core_threads <= 0 || single_core_threads > max_threads) {
            fprintf(stderr,
                    "Error: --single-core-threads=%d must be 1-%d\n",
                    single_core_threads, max_threads);
            return -1;
        }
        nthreads = single_core_threads;
    } else {
        nthreads = affinity;
    }

    /* NEW BEHAVIOR: Clamp instead of error */
    if (nthreads > max_threads) {
        fprintf(stderr,
                "Info: system shows %d CPUs but --max-threads=%d. "
                "Clamping thread count to %d.\n",
                nthreads, max_threads, max_threads);
        nthreads = max_threads;
    }

    if (nthreads > DEFAULT_MAX_THREADS)
        nthreads = DEFAULT_MAX_THREADS;

    /* SIMD capability checks (skip for AUTO as it's already validated) */
    if (type == W_AUTO) {
        /* AUTO type will be resolved to actual type before here */
        return 0;
    }
    
    if (type == W_SSE && !cpu_supports_sse()) {
        fprintf(stderr,
                "Error: CPU does not support SSE4.2.\n");
        return -1;
    }

    if (type == W_AVX && !cpu_supports_avx()) {
        fprintf(stderr,
                "Error: CPU or OS does not support AVX (XGETBV/xsave).\n");
        return -1;
    }

    if (type == W_AVX2 && !cpu_supports_avx2()) {
        fprintf(stderr,
                "Error: CPU does not support AVX2 or FMA.\n");
        return -1;
    }

    if (type == W_AVX512 && !cpu_supports_avx512()) {
        fprintf(stderr,
                "Error: CPU or OS does not support AVX-512F.\n");
        return -1;
    }

    if (type == W_MIXED && !cpu_supports_avx()) {
        fprintf(stderr,
                "Error: MIXED mode requires AVX support.\n");
        return -1;
    }

    /* Temperature sensor detection */
    *temp_path = find_temperature_input_path();
    if (!*temp_path) {
        fprintf(stderr,
                "Warning: Could not find CPU temp sensor. "
                "Thermal auto-stop disabled.\n");
    } else {
        double t = read_temperature(*temp_path);
        if (isnan(t))
            fprintf(stderr, "Warning: CPU temp sensor unreadable.\n");
    }

    /* cpufreq write checks */
    if (wants_cpufreq_write && geteuid() != 0) {
        fprintf(stderr,
                "Error: root required for CPU frequency settings.\n");
        return -1;
    }

    /* MIXED ratio must be provided */
    if (type == W_MIXED) {
        if (!mixed_ratio_str) {
            fprintf(stderr,
                    "Error: MIXED mode requires --mixed-ratio A:B:C\n");
            return -1;
        }

        mixed_ratio_t mr;
        if (parse_mixed_ratio(mixed_ratio_str, &mr) != 0) {
            fprintf(stderr,
                    "Error: invalid --mixed-ratio format\n");
            return -1;
        }

        g_mixed_ratio = mr;
    }

    *out_nthreads = nthreads;
    return 0;
}


/*******************************************************
 * CoreBurner — CHUNK 4 / 5
 *  - Corrected worker_thread (uses g_mixed_ratio weights)
 *  - Main runtime loop: spawn workers, monitor, logging
 *  - Dynamic frequency tuner
 *  - Graceful shutdown & summary
 *******************************************************/

/* Note:
 * Replace any earlier worker_thread() with this corrected version.
 * This implementation uses per-thread rand_r and g_mixed_ratio for MIXED.
 */

void *worker_thread(void *arg) {
    worker_arg_t *w = (worker_arg_t *)arg;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(w->cpu_id, &cpuset);

    /* Try to pin thread */
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        cpu_set_t cur;
        CPU_ZERO(&cur);
        if (sched_getaffinity(0, sizeof(cur), &cur) == 0) {
            int fallback = -1;
            for (int i = 0; i < CPU_SETSIZE; ++i)
                if (CPU_ISSET(i, &cur)) { fallback = i; break; }
            if (fallback >= 0) {
                CPU_ZERO(&cpuset);
                CPU_SET(fallback, &cpuset);
                pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
                w->cpu_id = fallback;
            }
        }
    }

    /* Local workload state */
    volatile uint64_t int_state = (uint64_t)(uintptr_t)w ^ 0xabcdef;
    volatile double float_state = (double)(w->cpu_id + 1) * 1.234567;
    
    /* Aligned buffers for SIMD operations */
    float sse_buf[4] __attribute__((aligned(16)));
    float avx_buf[8] __attribute__((aligned(32)));
    float avx512_buf[16] __attribute__((aligned(64)));
    
    for (int i = 0; i < 4; ++i) sse_buf[i] = (float)(i + w->cpu_id);
    for (int i = 0; i < 8; ++i) avx_buf[i] = (float)(i + w->cpu_id);
    for (int i = 0; i < 16; ++i) avx512_buf[i] = (float)(i + w->cpu_id);

    /* RNG seed per-thread */
    unsigned int rnd_seed = (unsigned int)(time(NULL) ^ (uintptr_t)w ^ (w->cpu_id * 7919));

    const long period_ns = CONTROL_PERIOD_MS * 1000000L;
    double util = w->target_util;
    if (util < 0) util = 0;
    if (util > 100) util = 100;

    long busy_ns = (long)round((util / 100.0) * period_ns);
    long sleep_ns = period_ns - busy_ns;

    struct timespec t0, t1;

    while (!stop_flag) {
        clock_gettime(CLOCK_MONOTONIC, &t0);

        if (busy_ns > 0) {
            for (;;) {
                /* execute workload */
                if (w->type == W_INT) {
                    int_work_unit(&int_state);
                } else if (w->type == W_FLOAT) {
                    float_work_unit(&float_state);
                } else if (w->type == W_SSE) {
                    sse_work_unit(sse_buf);
                } else if (w->type == W_AVX) {
                    avx_work_unit(avx_buf);
                } else if (w->type == W_AVX2) {
                    avx2_work_unit(avx_buf);
                } else if (w->type == W_AVX512) {
                    avx512_work_unit(avx512_buf);
                } else { /* W_MIXED with weights */
                    /* Use g_mixed_ratio to choose which unit to run */
                    int pick = 0;
                    if (g_mixed_ratio.total > 0) {
                        pick = rand_r(&rnd_seed) % g_mixed_ratio.total;
                        if (pick < g_mixed_ratio.r_int) {
                            int_work_unit(&int_state);
                        } else if (pick < (g_mixed_ratio.r_int + g_mixed_ratio.r_float)) {
                            float_work_unit(&float_state);
                        } else {
                            /* Use best available SIMD */
                            avx2_work_unit(avx_buf);
                        }
                    } else {
                        /* fallback: 1:1:1 */
                        int_work_unit(&int_state);
                        float_work_unit(&float_state);
                        avx2_work_unit(avx_buf);
                    }
                }

                __atomic_fetch_add(&w->ops_done, 1, __ATOMIC_RELAXED);

                clock_gettime(CLOCK_MONOTONIC, &t1);
                long elapsed = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);

                if (elapsed >= busy_ns || stop_flag)
                    break;
            }
        }

        if (sleep_ns > 0 && !stop_flag)
            safe_nanosleep(0, sleep_ns);
    }

    return NULL;
}

/* ---------------- main runtime logic (spawn threads, monitoring, logging) ---------------- */
int main_runtime(
    const char *mode,
    double util,
    long duration,
    workload_t type,
    int nthreads,
    double temp_threshold,
    const char *log_path,
    int log_interval,
    int log_append,
    int dynamic_freq,
    long *current_max_freq,
    char **temp_path_ptr,
    worker_arg_t **out_wargs,
    pthread_t **out_tids,
    int single_core_id)
{
    /* allocate worker structures */
    pthread_t *tids = calloc(nthreads, sizeof(pthread_t));
    worker_arg_t *wargs = calloc(nthreads, sizeof(worker_arg_t));
    if (!tids || !wargs) {
        fprintf(stderr, "Allocation failed for threads\n");
        return -1;
    }

    for (int i = 0; i < nthreads; ++i) {
        /* For single-core-multi mode, all threads go to the same core */
        if (str_case_equal(mode, "single-core-multi")) {
            wargs[i].cpu_id = single_core_id;
        } else {
            wargs[i].cpu_id = i % g_available_cpus;
        }
        wargs[i].target_util = util;
        wargs[i].type = type;
        __atomic_store_n(&wargs[i].ops_done, 0, __ATOMIC_RELAXED);
    }

    /* signal handlers already set up by caller if needed */

    /* spawn worker threads */
    for (int i = 0; i < nthreads; ++i) {
        if (pthread_create(&tids[i], NULL, worker_thread, &wargs[i]) != 0) {
            fprintf(stderr, "Failed to create thread %d: %s\n", i, strerror(errno));
            stop_flag = 1;
            break;
        }
    }

    /* start monitor thread */
    pthread_t mon_tid = 0;
    if (pthread_create(&mon_tid, NULL, monitor_thread, NULL) != 0) {
        fprintf(stderr, "Warning: failed to start monitor thread\n");
        mon_tid = 0;
    }

    /* logging set up */
    FILE *logf = NULL;
    FILE *summaryf = NULL;
    char *summary_path = NULL;
    uint64_t *prev_ops = NULL;
    int logging_enabled = 0;

    if (log_path) {
        logf = fopen(log_path, log_append ? "a" : "w");
        if (!logf) {
            fprintf(stderr, "Failed to open log file '%s' for writing: %s\n", log_path, strerror(errno));
            logging_enabled = 0;
        } else {
            logging_enabled = 1;
            if (!log_append) {
                time_t ts = time(NULL);
                safe_fprintf_flush(logf, "# coreburner log\n");
                safe_fprintf_flush(logf, "# mode=%s\n", mode);
                safe_fprintf_flush(logf, "# workload=%s\n", 
                    (type == W_INT) ? "INT" : 
                    (type == W_FLOAT) ? "FLOAT" : 
                    (type == W_SSE) ? "SSE" :
                    (type == W_AVX) ? "AVX" : 
                    (type == W_AVX2) ? "AVX2" :
                    (type == W_AVX512) ? "AVX512" :
                    (type == W_AUTO) ? "AUTO" : "MIXED");
                safe_fprintf_flush(logf, "# util=%.1f\n", util);
                safe_fprintf_flush(logf, "# threads=%d\n", nthreads);
                safe_fprintf_flush(logf, "# interval=%ds\n", log_interval);
                safe_fprintf_flush(logf, "# temp_threshold=%.1f\n", temp_threshold);
                safe_fprintf_flush(logf, "# start_time=%ld\n", (long)ts);
            }

            /* CSV header */
            safe_fprintf_flush(logf, "timestamp,elapsed_sec,cpu_temp");
            int cores_to_log = (g_available_cpus > MAX_CORES_TO_LOG) ? MAX_CORES_TO_LOG : g_available_cpus;
            for (int c = 0; c < cores_to_log; ++c) safe_fprintf_flush(logf, ",cpu%d_util,cpu%d_freq", c, c);
            if (g_available_cpus > cores_to_log) safe_fprintf_flush(logf, ",cpu_others_util,cpu_others_freq");
            for (int t = 0; t < nthreads; ++t) safe_fprintf_flush(logf, ",thread%d_ops_delta", t);
            safe_fprintf_flush(logf, "\n");

            fflush(logf);
            prev_ops = calloc(nthreads, sizeof(uint64_t));
            if (prev_ops) for (int t = 0; t < nthreads; ++t) prev_ops[t] = 0;

            summary_path = malloc(strlen(log_path) + 20);
            if (summary_path) { strcpy(summary_path, log_path); strcat(summary_path, ".summary.txt"); summaryf = fopen(summary_path, "w"); if (!summaryf) { free(summary_path); summary_path = NULL; } }
        }
    }

    time_t start = time(NULL);
    time_t end_time = start + duration;
    time_t now = start;

    /* prepare /proc.stat buffers for monitoring */
    uint64_t *total_prev = calloc(g_available_cpus, sizeof(uint64_t));
    uint64_t *idle_prev  = calloc(g_available_cpus, sizeof(uint64_t));
    uint64_t *total_curr = calloc(g_available_cpus, sizeof(uint64_t));
    uint64_t *idle_curr  = calloc(g_available_cpus, sizeof(uint64_t));
    if (!total_prev || !idle_prev || !total_curr || !idle_curr) {
        fprintf(stderr, "Memory allocation failed\n");
        stop_flag = 1;
    } else {
        if (read_proc_stat(total_prev, idle_prev, g_available_cpus) <= 0) {
            fprintf(stderr, "Failed to read /proc/stat initial snapshot\n");
            stop_flag = 1;
        }
    }

    int cores_to_log = (g_available_cpus > MAX_CORES_TO_LOG) ? MAX_CORES_TO_LOG : g_available_cpus;

    /* Statistics tracking */
    double temp_sum = 0.0;
    long freq_sum = 0;
    int temp_count = 0;
    int freq_count = 0;

    /* dynamic freq tracking */
    if (dynamic_freq && current_max_freq) {
        for (int c = 0; c < g_available_cpus; ++c) {
            long hz = 0;
            if (read_scaling_cur_freq(c, &hz) == 0) current_max_freq[c] = hz;
            else current_max_freq[c] = 0;
        }
    }

    /* Main monitoring & logging loop */
    while (!stop_flag && now < end_time) {
        for (int s = 0; s < log_interval && !stop_flag; ++s) sleep(1);
        if (stop_flag) break;

        int cpus_read = read_proc_stat(total_curr, idle_curr, g_available_cpus);
        if (cpus_read <= 0) cpus_read = g_available_cpus;

        double *util_pct = calloc(cpus_read, sizeof(double));
        long *freqs = calloc(cpus_read, sizeof(long));
        if (!util_pct || !freqs) { free(util_pct); free(freqs); break; }

        for (int c = 0; c < cpus_read; ++c) {
            uint64_t totald = total_curr[c] - total_prev[c];
            uint64_t idled  = idle_curr[c] - idle_prev[c];
            double usage = 0.0;
            if (totald > 0) usage = 100.0 * (double)(totald - idled) / (double)totald;
            util_pct[c] = usage;
            total_prev[c] = total_curr[c];
            idle_prev[c] = idle_curr[c];

            if (read_scaling_cur_freq(c, &freqs[c]) != 0) freqs[c] = 0;
        }

        double tempC = NAN;
        if (temp_path_ptr && *temp_path_ptr) tempC = read_temperature(*temp_path_ptr);

        /* Accumulate statistics */
        if (!isnan(tempC)) {
            temp_sum += tempC;
            temp_count++;
        }
        for (int c = 0; c < cpus_read; ++c) {
            if (freqs[c] > 0) {
                freq_sum += freqs[c];
                freq_count++;
            }
        }

        now = time(NULL);
        int elapsed_sec = (int)(now - start);

        /* Console output */
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

        /* Logging to CSV */
        if (logf && logging_enabled) {
            if (safe_fprintf_flush(logf, "%ld,%d,", (long)now, elapsed_sec) != 0) {
                fprintf(stderr, "Warning: log write failed. Disabling logging.\n");
                fclose(logf); logf = NULL; logging_enabled = 0;
            } else {
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

        /* Dynamic freq tuner */
        if (dynamic_freq && !isnan(tempC) && current_max_freq) {
            if (tempC >= temp_threshold) {
                for (int c = 0; c < g_available_cpus; ++c) {
                    long cur = current_max_freq[c];
                    if (cur <= 0) { long r=0; if (read_scaling_cur_freq(c, &r) == 0) cur = r; }
                    long new_max = (long)(cur * (100 - DYN_FREQ_STEP_PCT) / 100.0);
                    if (new_max < 100000) new_max = 100000;
                    if (write_scaling_min_max(c, -1, new_max) == 0) {
                        current_max_freq[c] = new_max;
                        fprintf(stderr, "dynamic-freq: reduced cpu %d max_freq to %ld kHz due to temp %.2f\n", c, new_max, tempC);
                    }
                }
            }
        }

        /* temp safety auto-stop */
        if (!isnan(tempC) && tempC >= temp_threshold) {
            fprintf(stderr, "ALERT: CPU temperature %.2f°C >= threshold %.2f°C. Stopping.\n", tempC, temp_threshold);
            stop_flag = 1;
            free(util_pct); free(freqs);
            break;
        }

        free(util_pct); free(freqs);
    }

    /* Stop workers and monitor */
    stop_flag = 1;
    for (int i = 0; i < nthreads; ++i) pthread_join(tids[i], NULL);
    if (mon_tid) pthread_join(mon_tid, NULL);

    /* Final summary with statistics */
    if (total_curr && idle_curr) read_proc_stat(total_curr, idle_curr, g_available_cpus);
    
    /* Calculate final statistics */
    uint64_t total_ops = 0;
    for (int t = 0; t < nthreads; ++t) {
        total_ops += __atomic_load_n(&wargs[t].ops_done, __ATOMIC_RELAXED);
    }
    long elapsed = (long)(time(NULL) - start);
    double avg_temp = temp_count > 0 ? temp_sum / temp_count : 0.0;
    long avg_freq = freq_count > 0 ? freq_sum / freq_count : 0;
    double avg_ops_per_core = nthreads > 0 ? (double)total_ops / nthreads : 0.0;
    double total_ops_millions = total_ops / 1000000.0;
    
    printf("\n=== SUMMARY ===\n");
    printf("\n--- Test Configuration ---\n");
    printf(" Mode            : %s\n", mode);
    printf(" Workload        : %s\n",
           (type==W_INT)?"INT":
           (type==W_FLOAT)?"FLOAT":
           (type==W_SSE)?"SSE":
           (type==W_AVX)?"AVX":
           (type==W_AVX2)?"AVX2":
           (type==W_AVX512)?"AVX512":
           (type==W_AUTO)?"AUTO":"MIXED");
    printf(" Threads         : %d\n", nthreads);
    printf(" Target Util     : %.1f%%\n", util);
    printf(" Duration        : %ld s (elapsed: %ld s)\n", duration, elapsed);
    
    printf("\n--- Aggregate Statistics ---\n");
    if (temp_count > 0)
        printf(" Avg Temperature : %.2f °C\n", avg_temp);
    else
        printf(" Avg Temperature : N/A\n");
    
    if (freq_count > 0)
        printf(" Avg Frequency   : %.2f MHz (%.2f GHz)\n", 
               avg_freq / 1000.0, avg_freq / 1000000.0);
    else
        printf(" Avg Frequency   : N/A\n");
    
    printf(" Total Operations: %.2f Million (%.2fM)\n", total_ops_millions, total_ops_millions);
    printf(" Avg Ops/Core    : %.2f Million\n", avg_ops_per_core / 1000000.0);
    printf(" Ops/Second      : %.2f Million/s\n", 
           elapsed > 0 ? total_ops_millions / elapsed : 0.0);
    
    if (temp_path_ptr && *temp_path_ptr) { 
        double t=read_temperature(*temp_path_ptr); 
        if (!isnan(t)) 
            printf(" Final Temperature: %.2f °C\n", t); 
    }
    
    printf("\n--- Per-Thread Details ---\n");
    for (int t = 0; t < nthreads; ++t) { 
        uint64_t ops = __atomic_load_n(&wargs[t].ops_done, __ATOMIC_RELAXED); 
        printf(" thread %2d -> cpu%2d : %" PRIu64 " ops (%.2fM)\n", 
               t, wargs[t].cpu_id, ops, ops / 1000000.0); 
    }

    /* Write summary file */
    if (summaryf) {
        fprintf(summaryf, "=== CoreBurner Test Summary ===\n\n");
        fprintf(summaryf, "[Configuration]\n");
        fprintf(summaryf, "mode=%s\n", mode);
        fprintf(summaryf, "workload=%s\n", 
            (type==W_INT)?"INT":
            (type==W_FLOAT)?"FLOAT":
            (type==W_SSE)?"SSE":
            (type==W_AVX)?"AVX":
            (type==W_AVX2)?"AVX2":
            (type==W_AVX512)?"AVX512":
            (type==W_AUTO)?"AUTO":"MIXED");
        fprintf(summaryf, "target_util=%.1f%%\n", util);
        fprintf(summaryf, "threads=%d\n", nthreads);
        fprintf(summaryf, "duration_requested=%ld\n", duration);
        fprintf(summaryf, "time_elapsed=%ld\n", elapsed);
        
        fprintf(summaryf, "\n[Aggregate Statistics]\n");
        if (temp_count > 0)
            fprintf(summaryf, "avg_temperature=%.2f\n", avg_temp);
        if (freq_count > 0)
            fprintf(summaryf, "avg_frequency_mhz=%.2f\n", avg_freq / 1000.0);
        fprintf(summaryf, "total_operations=%.2f\n", total_ops_millions);
        fprintf(summaryf, "avg_ops_per_core_millions=%.2f\n", avg_ops_per_core / 1000000.0);
        fprintf(summaryf, "ops_per_second_millions=%.2f\n", 
                elapsed > 0 ? total_ops_millions / elapsed : 0.0);
        
        if (temp_path_ptr && *temp_path_ptr) { 
            double t=read_temperature(*temp_path_ptr); 
            if (!isnan(t)) 
                fprintf(summaryf, "final_temp=%.2f\n", t); 
        }
        
        fprintf(summaryf, "\n[Per-Thread Results]\n");
        for (int t = 0; t < nthreads; ++t) { 
            uint64_t ops = __atomic_load_n(&wargs[t].ops_done, __ATOMIC_RELAXED); 
            fprintf(summaryf, "thread%02d_cpu%02d_ops=%" PRIu64 "\n", t, wargs[t].cpu_id, ops); 
            fprintf(summaryf, "thread%02d_cpu%02d_ops_millions=%.2f\n", t, wargs[t].cpu_id, ops / 1000000.0); 
        }
        fclose(summaryf);
        if (summary_path) printf("\nSummary written to %s\n", summary_path);
    }

    /* Cleanup */
    if (logf) fclose(logf);
    free(prev_ops);
    free(total_prev); free(idle_prev); free(total_curr); free(idle_curr);

    /* return allocated arrays to caller for potential further inspection */
    if (out_wargs) *out_wargs = wargs; else free(wargs);
    if (out_tids)  *out_tids  = tids;  else free(tids);

    return 0;
}

/*******************************************************
 * CoreBurner — CHUNK 5 / 5
 *  - main()
 *  - cpufreq write application
 *  - launch runtime
 *  - global cleanup
 *******************************************************/

int main(int argc, char **argv)
{
    char *mode = NULL;
    double util = -1;
    long duration = -1;
    workload_t type = W_INT;

    int max_threads = DEFAULT_MAX_THREADS;
    long duration_limit = DEFAULT_DURATION_LIMIT_SEC;
    double temp_threshold = DEFAULT_TEMP_THRESHOLD;
    int check_only = 0;

    char *log_path = NULL;
    int log_interval = DEFAULT_LOG_INTERVAL;
    int log_append = 0;

    char *set_governor = NULL;
    long set_min_freq = -1;
    long set_max_freq = -1;
    char *freq_table_str = NULL;

    int dynamic_freq = 0;
    char *mixed_ratio_str = NULL;
    int single_core_id = 0;
    int single_core_threads = 2;

    /* Parse CLI */
    if (parse_args(
            argc, argv,
            &mode, &util, &duration,
            &type,
            &max_threads, &duration_limit,
            &temp_threshold, &check_only,
            &log_path, &log_interval, &log_append,
            &set_governor, &set_min_freq, &set_max_freq,
            &freq_table_str,
            &dynamic_freq,
            &mixed_ratio_str,
            &single_core_id, &single_core_threads) != 0)
    {
        return 1;
    }

    /* Auto-detect best SIMD level if AUTO was specified */
    if (type == W_AUTO) {
        type = auto_detect_best_simd();
    }

    /* cpufreq/gov writes require root */
    int wants_cpufreq_write =
        (set_governor != NULL) ||
        (set_min_freq != -1) ||
        (set_max_freq != -1) ||
        (freq_table_str != NULL) ||
        (dynamic_freq);

    /* Validate environment */
    char *temp_path = NULL;
    int nthreads = 0;

if (validate_environment(
        mode,
        type,
        max_threads,
        &nthreads,
        &temp_path,
        temp_threshold,
        wants_cpufreq_write,
        mixed_ratio_str,
        single_core_id,
        single_core_threads
    ) != 0)
{
    free(temp_path);
    return 1;
}


    /* --check mode */
    if (check_only) {
        printf("✔ CHECK MODE: configuration validated.\n\n");
        printf("Planned configuration:\n");
        printf("  Mode            : %s\n", mode);
        printf("  Threads         : %d\n", nthreads);
        printf("  Workload        : %s\n",
               (type == W_INT) ? "INT" :
               (type == W_FLOAT) ? "FLOAT" :
               (type == W_SSE) ? "SSE" :
               (type == W_AVX) ? "AVX" :
               (type == W_AVX2) ? "AVX2" :
               (type == W_AVX512) ? "AVX512" :
               (type == W_AUTO) ? "AUTO" : "MIXED");
        printf("  Utilization     : %.1f%%\n", util);
        printf("  Duration        : %ld s\n", duration);

        if (set_governor)
            printf("  Governor        : %s\n", set_governor);

        if (set_min_freq != -1 || set_max_freq != -1)
            printf("  Min/Max freq    : min=%ld  max=%ld\n",
                   set_min_freq, set_max_freq);

        if (freq_table_str)
            printf("  Per-core freq   : %s\n", freq_table_str);

        if (dynamic_freq)
            printf("  Dynamic freq    : enabled\n");

        if (mixed_ratio_str)
            printf("  Mixed ratio     : %s\n", mixed_ratio_str);
        
        if (str_case_equal(mode, "single-core-multi"))
            printf("  Single core ID  : %d (with %d threads)\n",
                   single_core_id, single_core_threads);

        if (log_path)
            printf("  Log file        : %s\n", log_path);

        printf("\n✔ No workload executed (because --check).\n");
        free(temp_path);
        return 0;
    }

    /***************************************************************
     * Apply cpufreq writes (if requested)
     ***************************************************************/
    if (wants_cpufreq_write) {
        if (set_governor) {
            for (int c = 0; c < g_available_cpus; ++c)
                if (write_scaling_governor(c, set_governor) != 0)
                    fprintf(stderr, "Warning: failed to set governor on CPU %d\n", c);
        }

        if (set_min_freq != -1 || set_max_freq != -1) {
            for (int c = 0; c < g_available_cpus; ++c)
                if (write_scaling_min_max(c, set_min_freq, set_max_freq) != 0)
                    fprintf(stderr, "Warning: failed to set min/max freq on CPU %d\n", c);
        }

        /* Parse freq-table="0:3200000,1:2800000,..." */
        int ft_count = 0;
        int *ft_cpu = NULL;
        long *ft_freq = NULL;

        if (freq_table_str &&
            parse_freq_table(freq_table_str, &ft_count, &ft_cpu, &ft_freq) == 0)
        {
            for (int i = 0; i < ft_count; ++i) {
                int cpu = ft_cpu[i];
                long hz = ft_freq[i];

                if (cpu < 0 || cpu >= g_available_cpus) {
                    fprintf(stderr,
                            "Warning: freq-table CPU %d is out of range\n", cpu);
                    continue;
                }

                if (write_scaling_min_max(cpu, hz, hz) != 0)
                    fprintf(stderr,
                            "Warning: failed to set freq %ld on CPU %d\n",
                            hz, cpu);
            }

            free(ft_cpu);
            free(ft_freq);
        }
    }

    /***************************************************************
     * Prepare dynamic freq tuner storage
     ***************************************************************/
    long *current_max_freq = NULL;
    if (dynamic_freq) {
        current_max_freq = calloc(g_available_cpus, sizeof(long));
        if (!current_max_freq) {
            fprintf(stderr, "Memory allocation error\n");
            free(temp_path);
            return 1;
        }
    }

    /***************************************************************
     * Install signal handlers
     ***************************************************************/
    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);

    /***************************************************************
     * Launch main runtime
     ***************************************************************/
    worker_arg_t *wargs = NULL;
    pthread_t *tids = NULL;

    int rc = main_runtime(
                mode,
                util,
                duration,
                type,
                nthreads,
                temp_threshold,
                log_path,
                log_interval,ls

                log_append,
                dynamic_freq,
                current_max_freq,
                &temp_path,
                &wargs,
                &tids,
                single_core_id
            );

    /***************************************************************
     * Cleanup
     ***************************************************************/
    free(temp_path);
    free(current_max_freq);

    if (wargs) free(wargs);
    if (tids)  free(tids);

    return rc == 0 ? 0 : 1;
}


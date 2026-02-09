

          
# **CoreBurner — Advanced Linux CPU Stress, Telemetry & Frequency Validation Tool**

CoreBurner is a high-precision Linux workload generator built for **benchmarking**, **thermal testing**, **frequency scaling analysis**, **CPU stability validation**, and **DCL frequency validation**.  
It includes advanced workloads (INT / FLOAT / AVX / MIXED), full thread pinning, detailed telemetry, **active CPUFreq manipulation** (governor + per-core frequency tuning), and **comprehensive DCL/CWF frequency validation** for Intel processors.

## 🆕 NEW: DCL Frequency Validation

CoreBurner now supports comprehensive **Data Center List (DCL) frequency validation** for Intel CWF (Custom Workload Frequency) and other custom SKUs. This addresses the industry need for validating CPU frequencies against specifications, with built-in Cdyn class detection and automated pass/fail reporting.

**See [doc/DCL_VALIDATION.md](doc/DCL_VALIDATION.md) for complete documentation.**

### Quick Example:
```bash
# Validate AVX2 P0n frequency against DCL specification
./coreburner --mode multi --util 100 --duration 60s --type AVX2 \
  --dcl-avx2-freq 2800 --dcl-tolerance 3.0 --log avx2_validation.csv
```

---

##Features

###  Workload Engine
- Single-thread & multi-thread modes
- **Single-core multi-threaded mode** (for hyperthreading/SMT testing)
- Per-thread CPU pinning
- **Multiple SIMD instruction set levels** (auto-detected):
  - `INT` - Integer operations
  - `FLOAT` - Scalar floating-point
  - `SSE` - 128-bit SIMD (SSE4.2)
  - `AVX` - 256-bit FP SIMD
  - `AVX2` - 256-bit FP + INT with FMA
  - `AVX512` - 512-bit SIMD (AVX-512F)
  - `MIXED` - Combination workload (INT:FLOAT:SIMD ratios)
- Precise CPU utilization targeting (10–100%)

###  Real-Time Telemetry
- Per-core utilization (via `/proc/stat`)
- Per-core CPU frequency (`scaling_cur_freq`)
- CPU temperature sensors (hwmon/thermal_zone)
- Per-thread ops/sec tracking
- Console + CSV streaming output

### Thermal Controls
- Auto-stop at temperature threshold
- **Dynamic Frequency Tuner**
  - Automatically reduces max frequency when CPU overheats
  - Optional cooling hysteresis for stability

### CPUFreq Control (Requires root)
- Set CPU governor  
- Set min/max frequency  
- Per-core frequency map  
- Full control via:


## Logging
### CSV Columns:
- Timestamp
- Elapsed time
- Temperature
- Per-core utilization
- Per-core frequency
- Per-thread operation deltas

### Human-Readable Summary
`run.csv.summary.txt`  
Contains:
- Workload configuration  
- Duration  
- Temperature  
- Ops per thread  

---

## Build

```bash
gcc -O2 -march=native -pthread -std=c11 \
    -Wall -Wextra -o coreburner cpustress.c -lm
```
## Example Usage

### Full system load at 75% with AVX

This runs one worker per core at 75% utilization using AVX instructions and logs telemetry to `run.csv`.

```bash
./coreburner --mode multi --util 75 \
  --duration 2m --type AVX --log run.csv

```

### Test different SIMD instruction set levels

Compare power consumption and performance across SSE, AVX, AVX2, and AVX-512:

```bash
# SSE (128-bit) workload
./coreburner --mode single --util 95 --duration 30s \
  --type SSE --log sse_test.csv

# AVX (256-bit FP) workload
./coreburner --mode single --util 95 --duration 30s \
  --type AVX --log avx_test.csv

# AVX2 (256-bit with FMA) workload - higher power draw
./coreburner --mode single --util 95 --duration 30s \
  --type AVX2 --log avx2_test.csv

# AVX-512 (512-bit) workload - maximum power, possible frequency throttling
./coreburner --mode single --util 95 --duration 30s \
  --type AVX512 --log avx512_test.csv
```

### Single core with multiple threads (SMT/Hyperthreading test)

This runs 4 threads on CPU core 0 at 90% utilization to test thread contention and SMT performance.

```bash
./coreburner --mode single-core-multi --single-core-id 0 \
  --single-core-threads 4 --util 90 --duration 1m \
  --type MIXED --mixed-ratio 3:2:1 --log smt_test.csv
```

###Governor control + max frequency test

Requires root. Sets governor to performance, caps max frequency at 3.4 GHz, and runs INT workload.

```bash
sudo ./coreburner --mode multi --type INT \
  --duration 1m --set-governor performance \
  --set-max-freq 3400000
```
###Thermal stress with auto frequency tuning

Runs full load with dynamic frequency tuning, reducing max frequency if temperature exceeds 85°C.
```
sudo ./coreburner --mode multi --util 100 \
  --duration 3m --dynamic-freq --temp-threshold 85
```

---

## Verifying SIMD Instruction Usage

### Quick Check
```bash
# Show CPU capabilities and recommended workload
./coreburner --check-simd

# Auto-detect and use best SIMD level
./coreburner --mode multi --util 50 --duration 10 --type AUTO
```

### Comprehensive Verification
```bash
# Run verification script
./script/verify_simd.sh

# Check CPU flags
lscpu | grep -i flags
cat /proc/cpuinfo | grep flags | head -1

# Inspect binary for SIMD instructions
objdump -d coreburner | grep 'ymm'   # AVX/AVX2 (256-bit)
objdump -d coreburner | grep 'zmm'   # AVX-512 (512-bit)
objdump -d coreburner | grep vfmadd  # FMA instructions

# Look for specific work units
objdump -d coreburner | grep -A 20 '<avx2_work_unit>:'
```

### Runtime Profiling with perf
```bash
# Basic profiling (requires root)
sudo perf stat ./coreburner --mode multi --util 50 --duration 10 --type AVX2

# Detailed instruction analysis
sudo perf record -e cycles,instructions ./coreburner --mode multi --util 50 --duration 10 --type AVX2
sudo perf report

# SIMD-specific events (Intel CPUs)
sudo perf stat -e fp_arith_inst_retired.256b_packed_single,fp_arith_inst_retired.256b_packed_double ./coreburner --mode multi --util 50 --duration 10 --type AVX2

# Check FMA usage
sudo perf stat -e fp_arith_inst_retired.scalar_single,fp_arith_inst_retired.256b_packed_single ./coreburner --mode multi --util 50 --duration 10 --type AVX2
```

### What to Look For
- **SSE (128-bit)**: `xmm` registers, instructions like `movaps`, `mulps`, `addps`
- **AVX (256-bit)**: `ymm` registers, instructions like `vmovaps`, `vmulps`, `vaddps`
- **AVX2 (256-bit + FMA)**: `vfmadd` (fused multiply-add), `vbroadcastss`, integer vector ops
- **AVX-512 (512-bit)**: `zmm` registers, instructions like `vmulps %zmm`

---


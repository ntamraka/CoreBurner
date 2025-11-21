

          
# **CoreBurner — Advanced Linux CPU Stress, Telemetry & CPUFreq Control Tool**

CoreBurner is a high-precision Linux workload generator built for **benchmarking**, **thermal testing**, **frequency scaling analysis**, and **CPU stability validation**.  
It includes advanced workloads (INT / FLOAT / AVX / MIXED), full thread pinning, detailed telemetry, and **active CPUFreq manipulation** (governor + per-core frequency tuning).

---

## 🔥 Features

### 🧵 Workload Engine
- Single-thread & multi-thread modes
- Per-thread CPU pinning
- Workload types:
  - `INT`
  - `FLOAT`
  - `AVX` (auto-detect)
  - `MIXED` (combines all)
- Precise CPU utilization targeting (10–100%)

### 📡 Real-Time Telemetry
- Per-core utilization (via `/proc/stat`)
- Per-core CPU frequency (`scaling_cur_freq`)
- CPU temperature sensors (hwmon/thermal_zone)
- Per-thread ops/sec tracking
- Console + CSV streaming output

### 🧊 Thermal Controls
- Auto-stop at temperature threshold
- **Dynamic Frequency Tuner**
  - Automatically reduces max frequency when CPU overheats
  - Optional cooling hysteresis for stability

### 🛠 CPUFreq Control (Requires root)
- Set CPU governor  
- Set min/max frequency  
- Per-core frequency map  
- Full control via:


## 📊 Logging
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

## 🛠 Build

```bash
gcc -O2 -march=native -pthread -std=c11 \
    -Wall -Wextra -o coreburner cpustress.c -lm

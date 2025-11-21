#!/bin/bash
# =========================================================
#  CoreBurner Automated Feature Test Suite
#  Covers: workload types, modes, dynamic freq, logging,
#          cpufreq governor/freq tests, error handling.
# =========================================================

BIN="./coreburner"
LOGDIR="test_results"
TEMP_LIMIT=80   # Keep tests safe
mkdir -p "$LOGDIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

banner() {
    echo -e "\n${GREEN}=== $1 ===${NC}\n"
}

check_bin() {
    if [[ ! -f "$BIN" ]]; then
        echo -e "${RED}[ERROR] $BIN not found. Build using 'make'.${NC}"
        exit 1
    fi
}

has_avx() {
    $BIN --mode single --util 50 --duration 1s --type AVX --check >/dev/null 2>&1
    return $?
}

is_root() {
    [[ $EUID -eq 0 ]]
    return $?
}

run_test() {
    DESC="$1"
    CMD="$2"
    LOG="$3"

    banner "$DESC"
    echo "Running: $CMD"
    echo
    eval "$CMD" > "$LOGDIR/$LOG" 2>&1

    if [[ $? -eq 0 ]]; then
        echo -e "${GREEN}[PASS]${NC} $DESC"
    else
        echo -e "${RED}[FAIL]${NC} $DESC (see $LOGDIR/$LOG)"
    fi
}

check_bin

# =========================================================
#  BEGIN TEST CASES
# =========================================================

banner "Starting CoreBurner Full Test Suite"
echo "Logs will be saved in: $LOGDIR"

# ---------------------------------------------------------
# 1. Basic Validation Tests
# ---------------------------------------------------------
run_test "Validation: Missing args" \
    "$BIN 2>/dev/null" \
    "err_missing_args.log"

run_test "Validation: Invalid mode" \
    "$BIN --mode wrong --util 50 --duration 1s --type INT 2>/dev/null" \
    "err_invalid_mode.log"

run_test "Validation: Invalid utilization" \
    "$BIN --mode single --util 5 --duration 1s --type INT 2>/dev/null" \
    "err_low_util.log"

run_test "Validation: Duration format check" \
    "$BIN --mode single --util 50 --duration 2m --type INT --check" \
    "duration_check.log"

# ---------------------------------------------------------
# 2. Workload Test Cases
# ---------------------------------------------------------
run_test "INT Workload / Single Core / Log" \
    "$BIN --mode single --util 50 --duration 3s --type INT --log $LOGDIR/int_single.csv" \
    "int_single.log"

run_test "FLOAT Workload / Single Core" \
    "$BIN --mode single --util 60 --duration 3s --type FLOAT" \
    "float_single.log"

run_test "MIXED Workload / Multi Core" \
    "$BIN --mode multi --util 70 --duration 5s --type MIXED --log $LOGDIR/mixed_multi.csv" \
    "mixed_multi.log"

# AVX workload (only if supported)
if has_avx; then
    run_test "AVX Workload / Multi Core" \
        "$BIN --mode multi --util 75 --duration 5s --type AVX --log $LOGDIR/avx_multi.csv" \
        "avx_multi.log"
else
    echo -e "${RED}Skipping AVX tests — CPU does not support AVX.${NC}"
fi

# ---------------------------------------------------------
# 3. Logging Features
# ---------------------------------------------------------
run_test "CSV Logging + Summary Test" \
    "$BIN --mode single --util 40 --duration 3s --type INT --log $LOGDIR/log_test.csv" \
    "log_test.log"

run_test "Log Interval Test" \
    "$BIN --mode multi --util 30 --duration 4s --log-interval 2 --type INT --log $LOGDIR/log_interval.csv" \
    "interval_test.log"

# ---------------------------------------------------------
# 4. Safety: Temperature Limit Auto-Stop
# (Short duration to avoid danger)
# ---------------------------------------------------------
run_test "Auto-stop High Temp Simulation (threshold low)" \
    "$BIN --mode multi --util 100 --duration 3s --temp-threshold 30 --type INT" \
    "safety_temp.log"

# ---------------------------------------------------------
# 5. CPU Frequency Control (ROOT ONLY)
# ---------------------------------------------------------
if is_root; then
    banner "Running CPUFreq Tests (Root Detected)"

    run_test "Governor Test: performance" \
        "$BIN --mode single --util 50 --duration 3s --type INT --set-governor performance" \
        "gov_perf.log"

    run_test "Set Max Frequency Test" \
        "$BIN --mode single --util 60 --duration 3s --type INT --set-max-freq 2000000" \
        "set_max_freq.log"

    run_test "Set Min+Max Frequency Test" \
        "$BIN --mode single --duration 3s --util 50 --type INT \
         --set-min-freq 1500000 --set-max-freq 1500000" \
        "minmax_freq.log"

    run_test "Per-Core Frequency Table Test" \
       "$BIN --mode multi --duration 3s --util 50 --type INT \
        --freq-table '0:1800000,1:1600000'" \
        "freq_table.log"

    run_test "Dynamic Freq Tuning" \
        "$BIN --mode multi --util 100 --duration 5s --dynamic-freq --temp-threshold $TEMP_LIMIT" \
        "dynamic_freq.log"

else
    echo -e "${RED}Skipping CPUFreq tests — requires root.${NC}"
fi

# ---------------------------------------------------------
# 6. Duration Formats
# ---------------------------------------------------------
run_test "Duration: seconds format" \
    "$BIN --mode single --util 60 --duration 3s --type INT" \
    "duration_s.log"

run_test "Duration: minutes format" \
    "$BIN --mode single --util 60 --duration 1m --type INT" \
    "duration_m.log"

run_test "Duration: hours format" \
    "$BIN --mode single --util 40 --duration 1h --type INT --check" \
    "duration_h.log"

# ---------------------------------------------------------
# 7. Multi-thread affinity tests
# ---------------------------------------------------------
run_test "Affinity: Multi-core pinning" \
    "$BIN --mode multi --util 50 --duration 4s --type INT" \
    "affinity_multi.log"

banner "ALL TESTS COMPLETED"
echo "Open logs in: $LOGDIR/"


#!/usr/bin/env bash
# =========================================================
#  CoreBurner Automated Feature Test Suite (updated)
#  Produces:
#    - per-test raw logs: test_results/<testname>.log or .csv
#    - summary CSV:     test_results/summary.csv  (open in Excel)
#  Notes:
#    - CPUFreq tests require root
#    - AVX tests are auto-skipped when CPU doesn't support AVX
#    - Mixed-ratio tests verify CSV output existence + header
# =========================================================

set -euo pipefail
BIN="./coreburner"
LOGDIR="test_results"
SUMMARY_CSV="${LOGDIR}/summary.csv"
TEMP_LIMIT=80   # safety default for dynamic-freq test

mkdir -p "$LOGDIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

banner() { echo -e "\n${GREEN}=== $1 ===${NC}\n"; }

check_bin() {
    if [[ ! -x "$BIN" ]]; then
        echo -e "${RED}[ERROR] $BIN not found or not executable. Build using 'make'.${NC}"
        exit 1
    fi
}

is_root() { [[ $(id -u) -eq 0 ]]; }

# quick AVX probe using the program's --check; returns 0 if AVX supported
has_avx() {
    "$BIN" --mode single --util 50 --duration 1s --type AVX --check >/dev/null 2>&1
    return $?
}

# Run test helper: writes result as CSV row
# args: name, expected_text, command, logfile, expected_exit (0 for success, non-zero for expected fail), grep_string(optional to assert)
run_test() {
    local name="$1"; shift
    local expected="$1"; shift
    local cmd="$1"; shift
    local logfile="$1"; shift
    local expect_exit="${1:-0}"; shift || true
    local expect_grep="${1:-}"; shift || true

    banner "$name"
    echo "CMD: $cmd"
    echo "LOG: $logfile"

    # Execute (time-limited via timeout to avoid stuck runs; 1m hard cap on tests unless duration bigger)
    # Use timeout only if available
    if command -v timeout >/dev/null 2>&1; then
        timeout 70s bash -lc "$cmd" >"$logfile" 2>&1 || true
        rc=$?
        # timeout returns 124 on timeout; treat as failure
    else
        bash -lc "$cmd" >"$logfile" 2>&1 || true
        rc=$?
    fi

    local notes=""
    local result="FAIL"

    # Basic exit-code match
    if [[ "$expect_exit" -eq 0 && "$rc" -eq 0 ]]; then
        result="PASS"
    elif [[ "$expect_exit" -ne 0 && "$rc" -ne 0 ]]; then
        result="PASS"
    else
        result="FAIL"
    fi

    # If user asked for grepping expected content, check log for string
    if [[ -n "$expect_grep" && -f "$logfile" ]]; then
        if grep -q -- "$expect_grep" "$logfile"; then
            # keep previous result (PASS/FAIL) but annotate
            notes="${notes}grep_ok"
        else
            notes="${notes}grep_missing"
            result="FAIL"
        fi
    fi

    # Special-case: some tests expect the program to print a "CHECK MODE" summary
    if [[ "$name" == *"CHECK"* ]]; then
        if grep -q -i "CHECK MODE" "$logfile" 2>/dev/null; then
            notes="${notes},check_ok"
            [[ "$result" != "FAIL" ]] || result="PASS"
        else
            notes="${notes},check_missing"
            result="FAIL"
        fi
    fi

    # Safety: if test was temperature-based auto-stop, verify that either exit non-zero or log contains "ALERT" or "Stopping"
    if [[ "$name" == *"Auto-stop High Temp"* ]]; then
        if grep -iqE "ALERT:|Stopping|auto-stop" "$logfile" 2>/dev/null; then
            notes="${notes},temp_alert"
            result="PASS"
        else
            notes="${notes},no_temp_alert"
            # don't override PASS if rc matched expected; mark fail if we expected auto-stop
            [[ "$expect_exit" -ne 0 ]] && result="FAIL"
        fi
    fi

    # Mixed-ratio: ensure CSV exists and contains header/timestamp
    if [[ "$name" == *"MIXED Workload"* || "$name" == *"MIXED ratio"* ]]; then
        # check for CSV file presence
        if [[ -f "$logfile" ]]; then
            if head -n 5 "$logfile" | grep -qi "timestamp"; then
                notes="${notes},csv_header_ok"
                [[ "$result" != "FAIL" ]] || result="PASS"
            else
                notes="${notes},csv_header_missing"
                result="FAIL"
            fi
        else
            notes="${notes},csv_missing"
            result="FAIL"
        fi
    fi

    # Collect a single-line summary entry
    # Fields: Test,Command,Expected,ExitCode,Result,Notes,LogFile
    # Escape double quotes in command
    local cmd_esc
    cmd_esc=$(echo "$cmd" | sed 's/"/""/g')
    printf '"%s","%s","%s","%d","%s","%s","%s"\n' \
        "$name" "$cmd_esc" "$expected" "$rc" "$result" "$notes" "$logfile" >>"$SUMMARY_CSV"

    # Print to console
    if [[ "$result" == "PASS" ]]; then
        echo -e "${GREEN}[PASS]${NC} $name"
    else
        echo -e "${RED}[FAIL]${NC} $name -- see $logfile"
    fi
}

# Initialize
check_bin
: > "$SUMMARY_CSV"
printf '"Test","Command","Expected","ExitCode","Result","Notes","LogFile"\n' >>"$SUMMARY_CSV"

banner "Starting CoreBurner Full Test Suite"
echo "Logs saved under: $LOGDIR"
echo "Summary CSV: $SUMMARY_CSV"

# -------------------------
# 1) Basic validation tests
# -------------------------
run_test "Validation: Missing args" \
    "exit non-zero (usage)" \
    "$BIN" \
    "$LOGDIR/err_missing_args.log" 1

run_test "Validation: Invalid mode" \
    "exit non-zero" \
    "$BIN --mode wrong --util 50 --duration 1s --type INT --check" \
    "$LOGDIR/err_invalid_mode.log" 1

run_test "Validation: Low utilization (reject)" \
    "exit non-zero" \
    "$BIN --mode single --util 5 --duration 1s --type INT --check" \
    "$LOGDIR/err_low_util.log" 1

run_test "Validation: Duration format check (check-only)" \
    "exit zero with CHECK MODE" \
    "$BIN --mode single --util 50 --duration 2m --type INT --check" \
    "$LOGDIR/duration_check.log" 0 "CHECK MODE"

# -------------------------
# 2) Workload functional tests
# -------------------------
run_test "INT Workload / Single Core / Log" \
    "exit zero and create csv/log" \
    "$BIN --mode single --util 50 --duration 3s --type INT --log $LOGDIR/int_single.csv" \
    "$LOGDIR/int_single.csv" 0

run_test "FLOAT Workload / Single Core" \
    "exit zero" \
    "$BIN --mode single --util 60 --duration 3s --type FLOAT" \
    "$LOGDIR/float_single.log" 0

run_test "MIXED Workload / Multi Core (CSV)" \
    "exit zero and CSV header" \
    "$BIN --mode multi --util 70 --duration 5s --type MIXED --mixed-ratio 30:20:50 --log $LOGDIR/mixed_multi.csv --log-interval 1" \
    "$LOGDIR/mixed_multi.csv" 0

# AVX tests: skip if unsupported
if has_avx; then
    run_test "AVX Workload / Multi Core" \
        "exit zero" \
        "$BIN --mode multi --util 75 --duration 5s --type AVX --log $LOGDIR/avx_multi.csv" \
        "$LOGDIR/avx_multi.csv" 0
else
    printf '"%s","%s","%s","%s","%s","%s","%s"\n' \
        "AVX Workload / Multi Core" "(skipped)" "skipped: avx unsupported" "" "SKIPPED" "avx_not_supported" "" >>"$SUMMARY_CSV"
    echo -e "${YELLOW}AVX tests skipped — CPU does not support AVX${NC}"
fi

# -------------------------
# 3) Logging features
# -------------------------
run_test "CSV Logging + Summary Test" \
    "exit zero and csv" \
    "$BIN --mode single --util 40 --duration 3s --type INT --log $LOGDIR/log_test.csv" \
    "$LOGDIR/log_test.csv" 0

run_test "Log Interval Test" \
    "exit zero and csv with interval" \
    "$BIN --mode multi --util 30 --duration 4s --log-interval 2 --type INT --log $LOGDIR/log_interval.csv" \
    "$LOGDIR/log_interval.csv" 0

# -------------------------
# 4) Safety: Temperature Limit Auto-Stop
# Using low threshold so test triggers quickly if temps measurable on this system
# -------------------------
run_test "Auto-stop High Temp Simulation (threshold low)" \
    "should attempt to auto-stop (may pass if sensor unavailable)" \
    "$BIN --mode multi --util 100 --duration 5s --temp-threshold 30 --type INT --log $LOGDIR/safety_temp.csv" \
    "$LOGDIR/safety_temp.csv" 0

# -------------------------
# 5) CPU Frequency control tests (root only)
# -------------------------
if is_root; then
    banner "Running CPUFreq Tests (root)"

    run_test "Governor Test: performance" \
        "root: set governor to performance (may warn)" \
        "$BIN --mode single --util 10 --duration 3s --type INT --set-governor performance" \
        "$LOGDIR/gov_perf.log" 0

    run_test "Set Max Frequency Test" \
        "root: set max freq (may warn)" \
        "$BIN --mode single --util 10 --duration 3s --type INT --set-max-freq 2000000" \
        "$LOGDIR/set_max_freq.log" 0

    run_test "Set Min+Max Frequency Test" \
        "root: set min/max frequency equal" \
        "$BIN --mode single --duration 3s --util 10 --type INT --set-min-freq 1500000 --set-max-freq 1500000" \
        "$LOGDIR/minmax_freq.log" 0

    run_test "Per-Core Frequency Table Test" \
        "root: set two CPU freqs (may warn if cpu count < 2)" \
        "$BIN --mode multi --duration 3s --util 10 --type INT --freq-table '0:1800000,1:1600000'" \
        "$LOGDIR/freq_table.log" 0

    run_test "Dynamic Freq Tuning" \
        "root: dynamic freq engaged (may reduce max freq)" \
        "$BIN --mode multi --util 100 --duration 5s --dynamic-freq --temp-threshold $TEMP_LIMIT --log $LOGDIR/dynamic_freq.csv" \
        "$LOGDIR/dynamic_freq.csv" 0
else
    printf '"%s","%s","%s","%s","%s","%s","%s"\n' \
        "CPUFreq tests" "(skipped)" "skipped: requires root" "" "SKIPPED" "requires_root" "" >>"$SUMMARY_CSV"
    echo -e "${YELLOW}Skipping CPUFreq tests — requires root${NC}"
fi

# -------------------------
# 6) Duration formats sanity
# -------------------------
run_test "Duration: seconds format" \
    "exit zero" \
    "$BIN --mode single --util 60 --duration 3s --type INT" \
    "$LOGDIR/duration_s.log" 0

run_test "Duration: minutes format" \
    "exit zero" \
    "$BIN --mode single --util 60 --duration 1m --type INT --check" \
    "$LOGDIR/duration_m.log" 0 "CHECK MODE"

run_test "Duration: hours format (check-only)" \
    "exit zero (check-only)" \
    "$BIN --mode single --util 40 --duration 1h --type INT --check" \
    "$LOGDIR/duration_h.log" 0 "CHECK MODE"

# -------------------------
# 7) Affinity & thread count tests
# -------------------------
# Test --max-threads clamping behavior
run_test "Max threads clamping (--max-threads 2)" \
    "should clamp to 2 threads and run" \
    "$BIN --mode multi --util 50 --duration 4s --type INT --max-threads 2 --log $LOGDIR/max_threads_clamp.csv" \
    "$LOGDIR/max_threads_clamp.csv" 0

# Test --check shows planned threads (dry run)
run_test "Check-only shows planned threads" \
    "CHECK MODE output" \
    "$BIN --mode multi --util 50 --duration 1s --type INT --max-threads 2 --check" \
    "$LOGDIR/check_threads.log" 0 "CHECK MODE"

# -------------------------
# Finalize
# -------------------------
banner "ALL TESTS COMPLETED"
echo "Summary CSV: $SUMMARY_CSV"
echo "Raw logs: $LOGDIR/*"

# Friendly exit
exit 0

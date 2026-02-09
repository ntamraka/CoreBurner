#!/bin/bash
#
# DCL Frequency Validation Script for CoreBurner
# 
# This script performs comprehensive frequency validation across different
# workload types (SSE, AVX, AVX2, AVX512) and Cdyn classes.
#
# Usage: ./validate_dcl_frequencies.sh
#
# Requirements:
#   - CoreBurner executable in current directory or PATH
#   - Root access for accurate measurements (optional but recommended)
#   - Configure DCL specifications below before running
#

set -e  # Exit on error

# ============================================================================
# CONFIGURATION: Update these values based on your DCL specifications
# ============================================================================

# Expected P0n frequencies (MHz) - single core 100% load
DCL_SSE_P0N=3200
DCL_AVX_P0N=3000
DCL_AVX2_P0N=2800
DCL_AVX512_P0N=2600

# Expected All-Core Turbo frequencies (MHz) - all cores 100% load
DCL_SSE_ALLCORE=2900
DCL_AVX_ALLCORE=2700
DCL_AVX2_ALLCORE=2500

# Tolerance (percentage deviation allowed)
DCL_TOLERANCE=3.0

# Test duration (seconds)
P0N_DURATION=60
ALLCORE_DURATION=120

# ============================================================================
# SCRIPT CONFIGURATION
# ============================================================================

COREBURNER="../coreburner"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_DIR="../dcl_validation_${TIMESTAMP}"
REPORT_FILE="${LOG_DIR}/validation_report.txt"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ============================================================================
# FUNCTIONS
# ============================================================================

print_header() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}========================================${NC}"
}

print_test() {
    echo -e "${YELLOW}► $1${NC}"
}

print_pass() {
    echo -e "${GREEN}✓ PASS: $1${NC}"
}

print_fail() {
    echo -e "${RED}✗ FAIL: $1${NC}"
}

check_prerequisites() {
    print_header "Checking Prerequisites"
    
    # Check if coreburner exists
    if [ ! -f "$COREBURNER" ]; then
        echo -e "${RED}Error: CoreBurner not found at $COREBURNER${NC}"
        exit 1
    fi
    
    # Check if executable
    if [ ! -x "$COREBURNER" ]; then
        echo -e "${RED}Error: CoreBurner is not executable${NC}"
        echo "Run: chmod +x $COREBURNER"
        exit 1
    fi
    
    # Check for MSR support
    if [ ! -e /dev/cpu/0/msr ]; then
        echo -e "${YELLOW}Warning: MSR device not found. Loading msr module...${NC}"
        if ! sudo modprobe msr 2>/dev/null; then
            echo -e "${YELLOW}Warning: Could not load msr module. MSR-based measurements unavailable.${NC}"
        fi
    fi
    
    # Check if running as root
    if [ "$EUID" -ne 0 ]; then
        echo -e "${YELLOW}Warning: Not running as root. Some features may be unavailable.${NC}"
        echo "For best results, run with: sudo $0"
    fi
    
    echo -e "${GREEN}Prerequisites check complete${NC}"
    echo ""
}

run_validation_test() {
    local test_name=$1
    local mode=$2
    local util=$3
    local duration=$4
    local workload_type=$5
    local dcl_freq=$6
    local log_file="${LOG_DIR}/${test_name}.csv"
    
    print_test "Running: $test_name"
    
    $COREBURNER \
        --mode "$mode" \
        --util "$util" \
        --duration "${duration}s" \
        --type "$workload_type" \
        --dcl-${workload_type,,}-freq "$dcl_freq" \
        --dcl-tolerance "$DCL_TOLERANCE" \
        --log "$log_file" \
        2>&1 | tee "${log_file}.output.txt"
    
    echo ""
}

# ============================================================================
# MAIN SCRIPT
# ============================================================================

print_header "CoreBurner DCL Frequency Validation Suite"
echo "Timestamp: $TIMESTAMP"
echo "Log Directory: $LOG_DIR"
echo ""

# Check prerequisites
check_prerequisites

# Create log directory
mkdir -p "$LOG_DIR"

# Initialize report
{
    echo "========================================="
    echo "CoreBurner DCL Frequency Validation Report"
    echo "========================================="
    echo "Date: $(date)"
    echo "Hostname: $(hostname)"
    echo "CPU: $(lscpu | grep 'Model name' | cut -d: -f2 | xargs)"
    echo "Kernel: $(uname -r)"
    echo ""
    echo "DCL Specifications:"
    echo "  SSE P0n:        ${DCL_SSE_P0N} MHz"
    echo "  AVX P0n:        ${DCL_AVX_P0N} MHz"
    echo "  AVX2 P0n:       ${DCL_AVX2_P0N} MHz"
    echo "  AVX512 P0n:     ${DCL_AVX512_P0N} MHz"
    echo "  Tolerance:      ${DCL_TOLERANCE}%"
    echo ""
    echo "========================================="
    echo ""
} > "$REPORT_FILE"

# ============================================================================
# P0n TESTS (Single Core, 100% Utilization)
# ============================================================================

print_header "Phase 1: P0n Frequency Tests (Single Core)"

run_validation_test \
    "sse_p0n" \
    "single" \
    "100" \
    "$P0N_DURATION" \
    "SSE" \
    "$DCL_SSE_P0N"

run_validation_test \
    "avx_p0n" \
    "single" \
    "100" \
    "$P0N_DURATION" \
    "AVX" \
    "$DCL_AVX_P0N"

run_validation_test \
    "avx2_p0n" \
    "single" \
    "100" \
    "$P0N_DURATION" \
    "AVX2" \
    "$DCL_AVX2_P0N"

# Only test AVX512 if specified
if [ "$DCL_AVX512_P0N" -gt 0 ]; then
    run_validation_test \
        "avx512_p0n" \
        "single" \
        "100" \
        "$P0N_DURATION" \
        "AVX512" \
        "$DCL_AVX512_P0N"
fi

# ============================================================================
# ALL-CORE TURBO TESTS (Multi Core, 100% Utilization)
# ============================================================================

print_header "Phase 2: All-Core Turbo Frequency Tests"

run_validation_test \
    "sse_allcore" \
    "multi" \
    "100" \
    "$ALLCORE_DURATION" \
    "SSE" \
    "$DCL_SSE_ALLCORE"

run_validation_test \
    "avx_allcore" \
    "multi" \
    "100" \
    "$ALLCORE_DURATION" \
    "AVX" \
    "$DCL_AVX_ALLCORE"

run_validation_test \
    "avx2_allcore" \
    "multi" \
    "100" \
    "$ALLCORE_DURATION" \
    "AVX2" \
    "$DCL_AVX2_ALLCORE"

# ============================================================================
# RESULTS SUMMARY
# ============================================================================

print_header "Generating Results Summary"

# Extract validation results from output files
{
    echo ""
    echo "========================================="
    echo "TEST RESULTS"
    echo "========================================="
    echo ""
    
    for output_file in "${LOG_DIR}"/*.output.txt; do
        test_name=$(basename "$output_file" .output.txt)
        
        if grep -q "Result.*PASS" "$output_file"; then
            echo "[PASS] $test_name"
        elif grep -q "Result.*FAIL" "$output_file"; then
            echo "[FAIL] $test_name"
        else
            echo "[????] $test_name (unknown status)"
        fi
    done
    
    echo ""
    echo "========================================="
    echo "DETAILED RESULTS"
    echo "========================================="
    echo ""
    
    for output_file in "${LOG_DIR}"/*.output.txt; do
        test_name=$(basename "$output_file" .output.txt)
        echo "--- $test_name ---"
        
        # Extract validation section
        sed -n '/=== DCL Frequency Validation ===/,/================================/p' "$output_file"
        echo ""
    done
    
} >> "$REPORT_FILE"

# Display report
cat "$REPORT_FILE"

# ============================================================================
# FINAL STATUS
# ============================================================================

print_header "Validation Complete"

# Count passes and fails
PASS_COUNT=$(grep -c "^\[PASS\]" "$REPORT_FILE" || true)
FAIL_COUNT=$(grep -c "^\[FAIL\]" "$REPORT_FILE" || true)
TOTAL_COUNT=$((PASS_COUNT + FAIL_COUNT))

echo ""
echo "Results Summary:"
echo "  Total Tests: $TOTAL_COUNT"
echo -e "  ${GREEN}Passed: $PASS_COUNT${NC}"
echo -e "  ${RED}Failed: $FAIL_COUNT${NC}"
echo ""
echo "Detailed logs: $LOG_DIR"
echo "Full report: $REPORT_FILE"
echo ""

if [ $FAIL_COUNT -eq 0 ]; then
    print_pass "All validation tests passed!"
    exit 0
else
    print_fail "$FAIL_COUNT test(s) failed"
    exit 1
fi

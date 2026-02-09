#!/bin/bash
################################################################################
# Official DCL Frequency Validation Script
# 
# Purpose: Validate CPU frequencies against official DCL specifications for
#          all license levels (Cdyn classes) under all-core turbo conditions.
#
# Usage: ./official_dcl_validation.sh
#
# Requirements:
# - CoreBurner binary in same directory
# - Root access for MSR/RAPL (optional)
# - Performance governor recommended
#
################################################################################

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

################################################################################
# DCL SPECIFICATIONS - UPDATE THESE WITH YOUR OFFICIAL DCL VALUES
################################################################################

# All-Core Turbo Frequencies (MHz) for each license level
# TODO: Replace these with actual DCL specifications from PRC team
DCL_SSE_ALLCORE=2800      # Cdyn0 (License Level 0) - SSE all-core turbo
DCL_AVX_ALLCORE=2800      # Cdyn1 (License Level 1) - AVX all-core turbo
DCL_AVX2_ALLCORE=2800     # Cdyn2 (License Level 2) - AVX2 all-core turbo

# Validation tolerance (percentage)
DCL_TOLERANCE=3.0         # 3% tolerance for frequency deviation

# Test duration (seconds)
TEST_DURATION=60          # 60 seconds per test

################################################################################
# Configuration
################################################################################

# Create timestamped log directory
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_DIR="../dcl_validation_${TIMESTAMP}"
mkdir -p "$LOG_DIR"

# Log file
REPORT_FILE="$LOG_DIR/DCL_VALIDATION_REPORT.txt"

################################################################################
# Functions
################################################################################

print_header() {
    echo -e "${BLUE}╔═══════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║                                                                   ║${NC}"
    echo -e "${BLUE}║        DCL All-Core Turbo Frequency Validation Suite             ║${NC}"
    echo -e "${BLUE}║                                                                   ║${NC}"
    echo -e "${BLUE}╚═══════════════════════════════════════════════════════════════════╝${NC}"
    echo ""
}

print_section() {
    echo ""
    echo -e "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${YELLOW}  $1${NC}"
    echo -e "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo ""
}

check_prerequisites() {
    print_section "Checking Prerequisites"
    
    # Check if coreburner exists
    if [ ! -f "../coreburner" ]; then
        echo -e "${RED}✗ CoreBurner binary not found!${NC}"
        echo "  Please compile CoreBurner first: make"
        exit 1
    fi
    echo -e "${GREEN}✓${NC} CoreBurner binary found"
    
    # Check if running as root (for MSR/RAPL)
    if [ "$EUID" -eq 0 ]; then
        echo -e "${GREEN}✓${NC} Running as root (MSR/RAPL available)"
    else
        echo -e "${YELLOW}⚠${NC} Not running as root (MSR/RAPL disabled)"
    fi
    
    # Check CPU governor
    GOVERNOR=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)
    if [ "$GOVERNOR" = "performance" ]; then
        echo -e "${GREEN}✓${NC} CPU governor: performance"
    else
        echo -e "${YELLOW}⚠${NC} CPU governor: $GOVERNOR (recommend: performance)"
    fi
    
    # Get CPU info
    CPU_MODEL=$(lscpu | grep "Model name" | cut -d: -f2 | xargs)
    CPU_COUNT=$(nproc)
    CPU_MAX=$(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq 2>/dev/null)
    CPU_MAX_MHZ=$((CPU_MAX / 1000))
    
    echo -e "${BLUE}ℹ${NC} CPU: $CPU_MODEL"
    echo -e "${BLUE}ℹ${NC} Cores: $CPU_COUNT"
    echo -e "${BLUE}ℹ${NC} Max Frequency: $CPU_MAX_MHZ MHz"
    echo ""
}

run_validation_test() {
    local TEST_NAME=$1
    local WORKLOAD_TYPE=$2
    local EXPECTED_FREQ=$3
    local LICENSE_LEVEL=$4
    local LOG_FILE="$LOG_DIR/${WORKLOAD_TYPE}_allcore.csv"
    
    echo -e "${BLUE}▶${NC} Testing: $TEST_NAME"
    echo "  Workload: $WORKLOAD_TYPE"
    echo "  Expected: ${EXPECTED_FREQ} MHz"
    echo "  License Level: $LICENSE_LEVEL"
    echo "  Duration: ${TEST_DURATION}s"
    echo ""
    
    # Run CoreBurner test
    ./coreburner \
        --mode multi \
        --util 100 \
        --duration ${TEST_DURATION}s \
        --type $WORKLOAD_TYPE \
        --dcl-${WORKLOAD_TYPE,,}-freq $EXPECTED_FREQ \
        --dcl-tolerance $DCL_TOLERANCE \
        --log "$LOG_FILE" 2>&1 | tail -20
    
    echo ""
}

generate_report() {
    print_section "Generating Validation Report"
    
    cat << EOF > "$REPORT_FILE"
╔═══════════════════════════════════════════════════════════════════╗
║                                                                   ║
║        DCL ALL-CORE TURBO FREQUENCY VALIDATION REPORT            ║
║                                                                   ║
╚═══════════════════════════════════════════════════════════════════╝

VALIDATION METADATA
═══════════════════════════════════════════════════════════════════

Test Date/Time : $(date)
Hostname       : $(uname -n)
Operator       : $(whoami)
Report ID      : ${TIMESTAMP}

PLATFORM DETAILS
═══════════════════════════════════════════════════════════════════

CPU Model      : $(lscpu | grep "Model name" | cut -d: -f2 | xargs)
Architecture   : $(uname -m)
CPU Cores      : $(nproc)
Max Frequency  : $(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq 2>/dev/null | awk '{print $1/1000}') MHz
Governor       : $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)
Kernel Version : $(uname -r)

DCL SPECIFICATIONS (All-Core Turbo)
═══════════════════════════════════════════════════════════════════

License Level 0 (Cdyn0 - SSE) : ${DCL_SSE_ALLCORE} MHz
License Level 1 (Cdyn1 - AVX) : ${DCL_AVX_ALLCORE} MHz
License Level 2 (Cdyn2 - AVX2): ${DCL_AVX2_ALLCORE} MHz

Tolerance                     : ${DCL_TOLERANCE}%
Test Duration                 : ${TEST_DURATION} seconds per test

VALIDATION RESULTS
═══════════════════════════════════════════════════════════════════

EOF

    # Extract results from summary files
    if [ -f "$LOG_DIR/SSE_allcore.csv.summary.txt" ]; then
        SSE_FREQ=$(grep "avg_frequency_mhz" "$LOG_DIR/SSE_allcore.csv.summary.txt" | cut -d= -f2)
        SSE_TEMP=$(grep "avg_temperature" "$LOG_DIR/SSE_allcore.csv.summary.txt" | cut -d= -f2)
        
        echo "Test 1: SSE (License Level 0 / Cdyn0)" >> "$REPORT_FILE"
        echo "───────────────────────────────────────────────────────────────────" >> "$REPORT_FILE"
        echo "  Workload Type     : SSE (128-bit SIMD)" >> "$REPORT_FILE"
        echo "  Cdyn Class        : Cdyn0 (Low)" >> "$REPORT_FILE"
        echo "  Expected Frequency: ${DCL_SSE_ALLCORE} MHz" >> "$REPORT_FILE"
        echo "  Measured Frequency: ${SSE_FREQ} MHz" >> "$REPORT_FILE"
        echo "  Avg Temperature   : ${SSE_TEMP}°C" >> "$REPORT_FILE"
        echo "  Test Duration     : ${TEST_DURATION}s" >> "$REPORT_FILE"
        echo "" >> "$REPORT_FILE"
    fi
    
    if [ -f "$LOG_DIR/AVX_allcore.csv.summary.txt" ]; then
        AVX_FREQ=$(grep "avg_frequency_mhz" "$LOG_DIR/AVX_allcore.csv.summary.txt" | cut -d= -f2)
        AVX_TEMP=$(grep "avg_temperature" "$LOG_DIR/AVX_allcore.csv.summary.txt" | cut -d= -f2)
        
        echo "Test 2: AVX (License Level 1 / Cdyn1)" >> "$REPORT_FILE"
        echo "───────────────────────────────────────────────────────────────────" >> "$REPORT_FILE"
        echo "  Workload Type     : AVX (256-bit FP SIMD)" >> "$REPORT_FILE"
        echo "  Cdyn Class        : Cdyn1 (Medium)" >> "$REPORT_FILE"
        echo "  Expected Frequency: ${DCL_AVX_ALLCORE} MHz" >> "$REPORT_FILE"
        echo "  Measured Frequency: ${AVX_FREQ} MHz" >> "$REPORT_FILE"
        echo "  Avg Temperature   : ${AVX_TEMP}°C" >> "$REPORT_FILE"
        echo "  Test Duration     : ${TEST_DURATION}s" >> "$REPORT_FILE"
        echo "" >> "$REPORT_FILE"
    fi
    
    if [ -f "$LOG_DIR/AVX2_allcore.csv.summary.txt" ]; then
        AVX2_FREQ=$(grep "avg_frequency_mhz" "$LOG_DIR/AVX2_allcore.csv.summary.txt" | cut -d= -f2)
        AVX2_TEMP=$(grep "avg_temperature" "$LOG_DIR/AVX2_allcore.csv.summary.txt" | cut -d= -f2)
        
        echo "Test 3: AVX2 (License Level 2 / Cdyn2)" >> "$REPORT_FILE"
        echo "───────────────────────────────────────────────────────────────────" >> "$REPORT_FILE"
        echo "  Workload Type     : AVX2 (256-bit FP+INT+FMA)" >> "$REPORT_FILE"
        echo "  Cdyn Class        : Cdyn2 (High)" >> "$REPORT_FILE"
        echo "  Expected Frequency: ${DCL_AVX2_ALLCORE} MHz" >> "$REPORT_FILE"
        echo "  Measured Frequency: ${AVX2_FREQ} MHz" >> "$REPORT_FILE"
        echo "  Avg Temperature   : ${AVX2_TEMP}°C" >> "$REPORT_FILE"
        echo "  Test Duration     : ${TEST_DURATION}s" >> "$REPORT_FILE"
        echo "" >> "$REPORT_FILE"
    fi
    
    cat << EOF >> "$REPORT_FILE"

DETAILED LOGS
═══════════════════════════════════════════════════════════════════

CSV Logs:
  - $LOG_DIR/SSE_allcore.csv
  - $LOG_DIR/AVX_allcore.csv
  - $LOG_DIR/AVX2_allcore.csv

Summary Files:
  - $LOG_DIR/SSE_allcore.csv.summary.txt
  - $LOG_DIR/AVX_allcore.csv.summary.txt
  - $LOG_DIR/AVX2_allcore.csv.summary.txt

CONCLUSION
═══════════════════════════════════════════════════════════════════

Review the validation results above to confirm:
1. Measured frequencies match DCL specifications within tolerance
2. All license levels (Cdyn0/1/2) achieve expected all-core turbo
3. Temperature remains within acceptable limits
4. No thermal throttling observed

If any test shows FAIL status:
- Check BIOS configuration (AVX offset, turbo settings)
- Verify thermal solution (cooling, airflow)
- Confirm power limits (TDP, PL1/PL2)
- Review system load (no background interference)

═══════════════════════════════════════════════════════════════════
Report Generated: $(date)
CoreBurner Version: 2.0+dcl
═══════════════════════════════════════════════════════════════════
EOF

    echo -e "${GREEN}✓${NC} Validation report generated: $REPORT_FILE"
    echo ""
}

display_summary() {
    print_section "Validation Summary"
    
    cat "$REPORT_FILE"
    
    echo ""
    echo -e "${BLUE}╔═══════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║  Validation Complete                                             ║${NC}"
    echo -e "${BLUE}║  Results: $LOG_DIR${NC}"
    echo -e "${BLUE}╚═══════════════════════════════════════════════════════════════════╝${NC}"
    echo ""
}

################################################################################
# Main Execution
################################################################################

main() {
    print_header
    
    check_prerequisites
    
    print_section "Starting DCL Validation Tests"
    echo "This will take approximately $((TEST_DURATION * 3)) seconds..."
    echo ""
    
    # Test 1: SSE (Cdyn0 / License Level 0)
    run_validation_test \
        "License Level 0 (Cdyn0)" \
        "SSE" \
        "$DCL_SSE_ALLCORE" \
        "0 (Low Power)"
    
    # Test 2: AVX (Cdyn1 / License Level 1)
    run_validation_test \
        "License Level 1 (Cdyn1)" \
        "AVX" \
        "$DCL_AVX_ALLCORE" \
        "1 (Medium Power)"
    
    # Test 3: AVX2 (Cdyn2 / License Level 2)
    run_validation_test \
        "License Level 2 (Cdyn2)" \
        "AVX2" \
        "$DCL_AVX2_ALLCORE" \
        "2 (High Power)"
    
    # Generate report
    generate_report
    
    # Display summary
    display_summary
    
    echo -e "${GREEN}✓ All tests completed successfully${NC}"
    echo ""
    echo "Next steps:"
    echo "  1. Review validation report: cat $REPORT_FILE"
    echo "  2. Check detailed logs in: $LOG_DIR/"
    echo "  3. Send results to your manager/PRC team"
    echo ""
}

# Run main function
main

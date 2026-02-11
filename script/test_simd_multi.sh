#!/bin/bash
# Test script for multi-core SIMD workload comparison
# Tests SSE, AVX, AVX2, and AVX-512 at 100% utilization with enhanced compute-heavy workloads
# Usage: ./test_simd_multi.sh [emon]

# Parse arguments
USE_TMC=""
if [ "$1" == "emon" ]; then
    USE_TMC="yes"
    echo "TMC profiling enabled"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  MULTI-CORE SIMD WORKLOAD TEST (Enhanced Compute)"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "Testing SSE, AVX, AVX2, and AVX-512 workloads..."
echo "Mode: Multi-core (all available cores)"
echo "Duration: 2 minutes each"
echo "Utilization: 100%"
echo "Array size: 1M floats per thread"
echo "Loop iterations: 100 per array chunk"
if [ "$USE_TMC" == "yes" ]; then
    echo "TMC: Enabled with dynamic -a (workload) and -i (Run1) arguments"
fi
echo ""

# Rebuild with new workloads
echo "Building coreburner with enhanced workloads..."
cd ..
make clean > /dev/null 2>&1
make
if [ $? -ne 0 ]; then
    echo "❌ Build failed!"
    exit 1
fi
echo "✓ Build successful"
echo ""
cd script

# Get CPU count
NCPU=$(nproc)
echo "Detected ${NCPU} CPUs"
echo ""

# Test 1: SSE (128-bit SIMD)
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Test 1/4: SSE (128-bit) workload - Multi-core"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
if [ "$USE_TMC" == "yes" ]; then
    python3 /root/tmc/tmc.py -Z metrics2 -u -n -x ntamraka -d /root/tmc/coreburner -G coreburner -a SSE -i Run1 -c "../coreburner --mode multi --util 100 --duration 2m --type SSE"
else
    ../coreburner --mode multi --util 100 --duration 2m --type SSE
fi
echo ""

# Test 2: AVX (256-bit SIMD)
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Test 2/4: AVX (256-bit) workload - Multi-core"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
if [ "$USE_TMC" == "yes" ]; then
    python3 /root/tmc/tmc.py -Z metrics2 -u -n -x ntamraka -d /root/tmc/coreburner -G coreburner -a AVX -i Run1 -c "../coreburner --mode multi --util 100 --duration 2m --type AVX"
else
    ../coreburner --mode multi --util 100 --duration 2m --type AVX
fi
echo ""

# Test 3: AVX2 (256-bit with FMA)
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Test 3/4: AVX2 (256-bit + FMA) workload - Multi-core"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
if [ "$USE_TMC" == "yes" ]; then
    python3 /root/tmc/tmc.py -Z metrics2 -u -n -x ntamraka -d /root/tmc/coreburner -G coreburner -a AVX2 -i Run1 -c "../coreburner --mode multi --util 100 --duration 2m --type AVX2"
else
    ../coreburner --mode multi --util 100 --duration 2m --type AVX2
fi
echo ""

# Test 4: AVX-512 (512-bit SIMD)
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Test 4/4: AVX-512 (512-bit) workload - Multi-core"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
if [ "$USE_TMC" == "yes" ]; then
    python3 /root/tmc/tmc.py -Z metrics2 -u -n -x ntamraka -d /root/tmc/coreburner -G coreburner -a AVX512 -i Run1 -c "../coreburner --mode multi --util 100 --duration 2m --type AVX512"
else
    ../coreburner --mode multi --util 100 --duration 2m --type AVX512
fi
echo ""

# Display results
echo "═══════════════════════════════════════════════════════════════"
echo "  TEST RESULTS SUMMARY"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "Last 4 test results (SSE → AVX → AVX2 → AVX-512):"
echo ""
tail -4 results.csv | awk -F',' '{
    printf "%-8s: %8.2f Mops/s | %5.2f°C | %4.0f MHz | %7.2fM ops total\n", 
    $5, $14, $11, $12, $13
}'
echo ""
echo "Performance Comparison:"
tail -4 results.csv | awk -F',' '
BEGIN { print "Workload  | Ops/Sec  | vs SSE   | Temp   | Freq      | Per-Core" }
BEGIN { print "----------|----------|----------|--------|-----------|----------" }
NR==1 { sse=$14; sse_core=$15; printf "%-9s | %8.2f | baseline | %5.2f°C | %4.0f MHz | %7.2f\n", $5, $14, $11, $12, $15 }
NR==2 { printf "%-9s | %8.2f | %+7.2f%% | %5.2f°C | %4.0f MHz | %7.2f\n", $5, $14, ($14-sse)/sse*100, $11, $12, $15 }
NR==3 { printf "%-9s | %8.2f | %+7.2f%% | %5.2f°C | %4.0f MHz | %7.2f\n", $5, $14, ($14-sse)/sse*100, $11, $12, $15 }
NR==4 { printf "%-9s | %8.2f | %+7.2f%% | %5.2f°C | %4.0f MHz | %7.2f\n", $5, $14, ($14-sse)/sse*100, $11, $12, $15 }
'
echo ""
echo "Theoretical vs Actual:"
tail -4 results.csv | awk -F',' '
NR==1 { sse=$14 }
NR==2 { avx=$14; printf "AVX vs SSE:    Theoretical 2.0x, Actual %.2fx (%.1f%% efficiency)\n", avx/sse, (avx/sse)/2.0*100 }
NR==3 { avx2=$14; printf "AVX2 vs SSE:   Theoretical 2.0x, Actual %.2fx (%.1f%% efficiency)\n", avx2/sse, (avx2/sse)/2.0*100 }
NR==4 { avx512=$14; printf "AVX512 vs SSE: Theoretical 4.0x, Actual %.2fx (%.1f%% efficiency)\n", avx512/sse, (avx512/sse)/4.0*100 }
'
echo ""
echo "✓ All tests completed!"

#!/bin/bash
# Compare different SIMD instruction set levels

echo "========================================"
echo "CoreBurner SIMD Level Comparison Test"
echo "========================================"
echo ""

# Test configuration
UTIL=90
DURATION=10s
MODE=multi

echo "Configuration: --mode $MODE --util $UTIL --duration $DURATION"
echo ""

# Test SSE
echo "[1/4] Testing SSE (128-bit)..."
./coreburner --mode $MODE --util $UTIL --duration $DURATION --type SSE --log sse_multi.csv 2>&1 | grep -A 15 "Aggregate Statistics"
echo ""

# Test AVX
echo "[2/4] Testing AVX (256-bit FP)..."
./coreburner --mode $MODE --util $UTIL --duration $DURATION --type AVX --log avx_multi.csv 2>&1 | grep -A 15 "Aggregate Statistics"
echo ""

# Test AVX2
echo "[3/4] Testing AVX2 (256-bit FP+INT+FMA)..."
./coreburner --mode $MODE --util $UTIL --duration $DURATION --type AVX2 --log avx2_multi.csv 2>&1 | grep -A 15 "Aggregate Statistics"
echo ""

# Test INT for baseline
echo "[4/4] Testing INT (baseline)..."
./coreburner --mode $MODE --util $UTIL --duration $DURATION --type INT --log int_multi.csv 2>&1 | grep -A 15 "Aggregate Statistics"
echo ""

echo "========================================"
echo "Comparison Summary"
echo "========================================"
echo ""
echo "Summary files created:"
ls -lh *_multi.csv.summary.txt 2>/dev/null | awk '{print $9, $5}'
echo ""
echo "To view detailed results:"
echo "  cat sse_multi.csv.summary.txt"
echo "  cat avx2_multi.csv.summary.txt"

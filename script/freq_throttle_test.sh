#!/bin/bash
# Test frequency throttling with different SIMD levels

echo "========================================"
echo "CoreBurner Frequency Throttling Test"
echo "Compare frequency behavior across SIMD levels"
echo "========================================"
echo ""

# Configuration
UTIL=100
DURATION=30s

echo "Running full load (${UTIL}%) for ${DURATION} each..."
echo ""

# SSE - baseline, no throttling expected
echo "[1/3] SSE workload (minimal throttling)..."
../coreburner --mode multi --util $UTIL --duration $DURATION --type SSE \
    --log throttle_sse.csv 2>&1 | grep -E "(Avg Frequency|Avg Temperature|Ops/Second)"
echo ""

# AVX2 - moderate throttling possible
echo "[2/3] AVX2 workload (moderate throttling)..."
../coreburner --mode multi --util $UTIL --duration $DURATION --type AVX2 \
    --log throttle_avx2.csv 2>&1 | grep -E "(Avg Frequency|Avg Temperature|Ops/Second)"
echo ""

# INT - check baseline performance
echo "[3/3] INT workload (no SIMD throttling)..."
../coreburner --mode multi --util $UTIL --duration $DURATION --type INT \
    --log throttle_int.csv 2>&1 | grep -E "(Avg Frequency|Avg Temperature|Ops/Second)"
echo ""

echo "========================================"
echo "Analysis:"
echo "- Lower frequency with AVX2 = frequency throttling"
echo "- Higher temperature = more power consumption"
echo "- Check CSV files for per-second frequency data"
echo ""
echo "Files created:"
ls -lh throttle_*.csv | awk '{print $9, $5}'

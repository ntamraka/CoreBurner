#!/bin/bash
# Power/Thermal testing across different workload intensities and SIMD levels

echo "========================================"
echo "CoreBurner Power/Thermal Testing"
echo "Multi-core mode with varying loads"
echo "========================================"
echo ""

# Test different utilization levels with AVX2 (highest power)
for util in 50 75 90 100; do
    echo "Testing AVX2 at ${util}% utilization..."
    ./coreburner --mode multi --util $util --duration 15s --type AVX2 \
        --log power_avx2_${util}pct.csv 2>&1 | grep -A 10 "Aggregate Statistics"
    echo ""
    sleep 5  # Cool down between tests
done

echo "========================================"
echo "Power test results:"
ls -lh power_*.csv.summary.txt | awk '{print $9, $5}'

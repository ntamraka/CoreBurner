#!/bin/bash
# Test script for single-core SIMD workload comparison
# Tests SSE, AVX, and AVX2 at 100% utilization with enhanced compute-heavy workloads

echo "═══════════════════════════════════════════════════════════════"
echo "  SINGLE-CORE SIMD WORKLOAD TEST (Enhanced Compute)"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "Testing SSE, AVX, and AVX2 workloads..."
echo "Duration: 2 minutes each"
echo "Utilization: 100%"
echo "Loop iterations: 10 million per work unit"
echo ""

# Rebuild with new workloads
echo "Building coreburner with enhanced workloads..."
make clean > /dev/null 2>&1
make
if [ $? -ne 0 ]; then
    echo "❌ Build failed!"
    exit 1
fi
echo "✓ Build successful"
echo ""

# Test 1: SSE (128-bit SIMD)
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Test 1/3: SSE (128-bit) workload"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
./coreburner --mode single --util 100 --duration 2m --type SSE
echo ""

# Test 2: AVX (256-bit SIMD)
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Test 2/3: AVX (256-bit) workload"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
./coreburner --mode single --util 100 --duration 2m --type AVX
echo ""

# Test 3: AVX2 (256-bit with FMA)
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Test 3/3: AVX2 (256-bit + FMA) workload"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
./coreburner --mode single --util 100 --duration 2m --type AVX2
echo ""

# Display results
echo "═══════════════════════════════════════════════════════════════"
echo "  TEST RESULTS SUMMARY"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "Last 3 test results (SSE → AVX → AVX2):"
echo ""
tail -3 results.csv | awk -F',' '{
    printf "%-6s: %7.2f Mops/s | %5.2f°C | %4.0f MHz | %.2fM ops total\n", 
    $5, $14, $11, $12, $13
}'
echo ""
echo "Comparison:"
tail -3 results.csv | awk -F',' '
BEGIN { print "Workload | Ops/Sec | vs SSE | Temp | Freq" }
BEGIN { print "---------|---------|--------|------|------" }
NR==1 { sse=$14; printf "%-8s | %7.2f | baseline | %5.2f°C | %4.0f MHz\n", $5, $14, $11, $12 }
NR==2 { printf "%-8s | %7.2f | %+6.2f%% | %5.2f°C | %4.0f MHz\n", $5, $14, ($14-sse)/sse*100, $11, $12 }
NR==3 { printf "%-8s | %7.2f | %+6.2f%% | %5.2f°C | %4.0f MHz\n", $5, $14, ($14-sse)/sse*100, $11, $12 }
'
echo ""
echo "✓ All tests completed!"

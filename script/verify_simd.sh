#!/bin/bash
# CoreBurner SIMD Verification Script
# This script verifies what SIMD instructions are available and being used

set -e

echo "================================================================"
echo "  CoreBurner SIMD Instruction Verification"
echo "================================================================"
echo ""

# 1. Check CPU capabilities
echo "1. CPU SIMD Support (from /proc/cpuinfo):"
echo "   ----------------------------------------"
cat /proc/cpuinfo | grep flags | head -1 | tr ' ' '\n' | grep -E '(sse|avx|fma)' | sort -u | sed 's/^/   ✓ /'
echo ""

# 2. Check what CoreBurner was compiled with
echo "2. CoreBurner Compile-time Detection:"
echo "   ----------------------------------------"
./coreburner --check-simd 2>&1 | grep -A 10 "Compile-time Flags"
echo ""

# 3. Check binary for SIMD instructions
echo "3. Binary Instruction Analysis:"
echo "   ----------------------------------------"

SSE_COUNT=$(objdump -d coreburner | grep -c 'xmm' || true)
AVX_YMM_COUNT=$(objdump -d coreburner | grep -c 'ymm' || true)
AVX512_ZMM_COUNT=$(objdump -d coreburner | grep -c 'zmm' || true)
FMA_COUNT=$(objdump -d coreburner | grep -c 'vfmadd' || true)

echo "   SSE instructions (xmm):        $SSE_COUNT occurrences"
echo "   AVX/AVX2 instructions (ymm):   $AVX_YMM_COUNT occurrences"
echo "   AVX-512 instructions (zmm):    $AVX512_ZMM_COUNT occurrences"
echo "   FMA instructions (vfmadd):     $FMA_COUNT occurrences"
echo ""

# 4. Show actual instructions from work units
echo "4. Work Unit Instructions (sample):"
echo "   ----------------------------------------"

echo "   AVX2 Work Unit:"
objdump -d coreburner | grep -A 8 '<avx2_work_unit>:' | grep -E '(vmov|vfmadd|vbroadcast)' | head -5 | sed 's/^/   /'

echo ""
echo "   AVX Work Unit:"
objdump -d coreburner | grep -A 8 '<avx_work_unit>:' | grep -E '(vmul|vadd|vmov)' | head -5 | sed 's/^/   /'

echo ""
echo "   SSE Work Unit:"
objdump -d coreburner | grep -A 8 '<sse_work_unit>:' | grep -E '(mul|add|mov)' | head -5 | sed 's/^/   /'

echo ""

# 5. Recommendations
echo "5. Recommendations:"
echo "   ----------------------------------------"
if [ $FMA_COUNT -gt 0 ]; then
    echo "   ✓ Best workload: --type AVX2 (FMA instructions available)"
elif [ $AVX_YMM_COUNT -gt 0 ]; then
    echo "   ✓ Best workload: --type AVX (256-bit vectors available)"
elif [ $SSE_COUNT -gt 0 ]; then
    echo "   ✓ Best workload: --type SSE (128-bit vectors available)"
else
    echo "   ✓ Best workload: --type FLOAT or INT (scalar only)"
fi
echo ""

# 6. Performance profiling commands
echo "6. To Profile at Runtime (requires perf):"
echo "   ----------------------------------------"
echo "   # Basic profiling:"
echo "   sudo perf stat ./coreburner --mode multi --util 50 --duration 10 --type AVX2"
echo ""
echo "   # Detailed instruction profiling:"
echo "   sudo perf record -e cycles,instructions ./coreburner --mode multi --util 50 --duration 10 --type AVX2"
echo "   sudo perf report"
echo ""
echo "   # SIMD-specific events (Intel):"
echo "   sudo perf stat -e fp_arith_inst_retired.256b_packed_single,fp_arith_inst_retired.256b_packed_double ./coreburner --mode multi --util 50 --duration 10 --type AVX2"
echo ""

echo "================================================================"
echo "Verification complete!"
echo "================================================================"

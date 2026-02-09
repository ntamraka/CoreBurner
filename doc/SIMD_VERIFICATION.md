# SIMD Instruction Verification Guide for CoreBurner

## Quick Start

### 1. Check What Your CPU Supports
```bash
./coreburner --check-simd
```

This shows:
- CPU architecture (x86_64/i386)
- Supported SIMD levels (SSE, AVX, AVX2, AVX-512)
- Compile-time flags enabled
- Recommended workload type
- Verification commands

### 2. Run Automated Verification
```bash
./verify_simd.sh
```

This script:
- Checks CPU flags from /proc/cpuinfo
- Analyzes the compiled binary for SIMD instructions
- Counts instruction occurrences
- Shows sample instructions from work units
- Provides profiling commands

## Understanding SIMD Levels

| Level    | Register Size | Registers | Example Instructions | Use Case |
|----------|--------------|-----------|---------------------|----------|
| SSE4.2   | 128-bit      | xmm0-xmm15| movaps, mulps, addps | Basic vectorization |
| AVX      | 256-bit      | ymm0-ymm15| vmovaps, vmulps     | 2x throughput vs SSE |
| AVX2     | 256-bit      | ymm0-ymm15| vfmadd, vbroadcast  | FMA + integer vectors |
| AVX-512  | 512-bit      | zmm0-zmm31| vmulps %zmm         | 4x throughput vs SSE |

## Verification Methods

### Method 1: Built-in Detection (Easiest)
```bash
./coreburner --check-simd
```

### Method 2: Check CPU Flags
```bash
# Full flags list
lscpu | grep Flags

# SIMD-specific flags
cat /proc/cpuinfo | grep flags | head -1 | tr ' ' '\n' | grep -E '(sse|avx|fma)'
```

Look for:
- `sse4_2` - SSE 4.2
- `avx` - AVX
- `avx2` - AVX2
- `fma` - Fused Multiply-Add (part of AVX2)
- `avx512f` - AVX-512 Foundation

### Method 3: Inspect Binary (objdump)
```bash
# Count SIMD register usage
objdump -d coreburner | grep -c 'xmm'   # SSE
objdump -d coreburner | grep -c 'ymm'   # AVX/AVX2
objdump -d coreburner | grep -c 'zmm'   # AVX-512

# Find FMA instructions
objdump -d coreburner | grep vfmadd

# Examine specific work unit
objdump -d coreburner | grep -A 20 '<avx2_work_unit>:'
```

Example AVX2 output:
```
3d60:  c5 fc 10 07          vmovups (%rdi),%ymm0      # Load 256-bit
3d80:  c4 e2 75 98 c2       vfmadd132ps %ymm2,%ymm1,%ymm0  # FMA
3d8f:  c5 fc 11 07          vmovups %ymm0,(%rdi)      # Store 256-bit
```

### Method 4: Runtime Profiling (perf)

**Basic profiling:**
```bash
sudo perf stat ./coreburner --mode multi --util 50 --duration 10 --type AVX2
```

**Detailed instruction capture:**
```bash
sudo perf record -e cycles,instructions ./coreburner --mode multi --util 50 --duration 10 --type AVX2
sudo perf report
```

**SIMD-specific counters (Intel):**
```bash
# Count 256-bit packed single-precision operations
sudo perf stat -e fp_arith_inst_retired.256b_packed_single \
  ./coreburner --mode multi --util 50 --duration 10 --type AVX2

# Compare scalar vs vectorized operations
sudo perf stat -e '{fp_arith_inst_retired.scalar_single,fp_arith_inst_retired.256b_packed_single}' \
  ./coreburner --mode multi --util 50 --duration 10 --type AVX2
```

**Available perf events (Intel):**
- `fp_arith_inst_retired.scalar_single` - Scalar FP ops
- `fp_arith_inst_retired.128b_packed_single` - SSE ops
- `fp_arith_inst_retired.256b_packed_single` - AVX/AVX2 ops
- `fp_arith_inst_retired.512b_packed_single` - AVX-512 ops

### Method 5: Check Compilation Flags
```bash
# Verify the binary was compiled with -march=native
strings coreburner | grep -i gcc
file coreburner
```

## Interpreting Results

### From --check-simd:
```
CPU Architecture Support:
  SSE4.2          : ✓ YES    <- CPU supports 128-bit SIMD
  AVX (256-bit)   : ✓ YES    <- CPU supports 256-bit FP
  AVX2 + FMA      : ✓ YES    <- Best choice for this CPU
  AVX-512         : ✗ NO     <- Not available

Compile-time Flags:
  __AVX2__        : Defined  <- Binary includes AVX2 instructions
  __FMA__         : Defined  <- Binary includes FMA instructions
```

### From objdump:
```bash
# If you see these patterns, the binary is using the right instructions:

# AVX2 with FMA:
c4 e2 75 98 c2    vfmadd132ps %ymm2,%ymm1,%ymm0   # Good!

# AVX (256-bit):
c5 fc 59 c1       vmulps %ymm1,%ymm0,%ymm0        # Good!

# SSE (128-bit):
0f 59 c1          mulps %xmm1,%xmm0               # Scalar fallback
```

### From perf:
```bash
# Good AVX2 utilization shows:
Performance counter stats:
  10,234,567,890  fp_arith_inst_retired.256b_packed_single  # High number = good
      12,345,678  fp_arith_inst_retired.scalar_single       # Low number = good
```

## Common Issues

### Issue: Binary not using expected SIMD level
**Cause:** Not compiled with `-march=native`
**Solution:**
```bash
make clean
make  # Uses -march=native in Makefile
```

### Issue: CPU supports AVX2 but workload seems slow
**Cause:** Frequency throttling (AVX2 can trigger lower turbo)
**Check:**
```bash
./coreburner --mode multi --util 50 --duration 10 --type AVX2 --log test.csv
# Watch the frequency in output
```

### Issue: AVX-512 reported as unavailable
**Cause:** OS may disable it, or CPU thermal limits
**Check:**
```bash
cat /proc/cpuinfo | grep avx512
dmesg | grep -i avx
```

## Performance Expectations

For the same workload intensity:
- AVX-512 ≈ 4x operations per cycle vs SSE
- AVX2 ≈ 2x operations per cycle vs SSE
- AVX2 with FMA ≈ 2.5-3x operations per cycle vs SSE

But note: Higher SIMD levels may trigger frequency throttling!

## Additional Tools

### Intel VTune Profiler
```bash
vtune -collect hotspots -- ./coreburner --mode multi --util 50 --duration 10 --type AVX2
```

### AMD uProf
```bash
AMDuProfCLI collect --event instruction-retired -- ./coreburner --mode multi --util 50 --duration 10 --type AVX2
```

### likwid-perfctr
```bash
likwid-perfctr -C 0-3 -g FLOPS_AVX ./coreburner --mode multi --util 50 --duration 10 --type AVX2
```

## Summary

**Easiest verification:**
```bash
./coreburner --check-simd
./verify_simd.sh
```

**Most detailed verification:**
```bash
objdump -d coreburner | grep -E '(ymm|vfmadd)' | wc -l
sudo perf stat ./coreburner --mode multi --util 50 --duration 10 --type AVX2
```

**Best practice:**
```bash
# Let CoreBurner auto-detect the best SIMD level
./coreburner --mode multi --util 50 --duration 30 --type AUTO
```

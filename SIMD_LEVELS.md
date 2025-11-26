# SIMD Instruction Set Support in CoreBurner

CoreBurner now supports testing multiple SIMD instruction set "license levels" for power management and performance validation.

## Supported SIMD Levels

| Level | Vector Width | Instructions | Feature Detection | Use Case |
|-------|-------------|--------------|-------------------|----------|
| **SSE** | 128-bit | SSE4.2 | CPUID leaf 1, ECX bit 20 | Legacy baseline, lowest power |
| **AVX** | 256-bit | FP only | CPUID + XCR0 check | Sandy Bridge+ (2011) |
| **AVX2** | 256-bit | FP + INT + FMA | CPUID leaf 7 + FMA bit | Haswell+ (2013), higher power |
| **AVX-512** | 512-bit | Full SIMD + masks | CPUID leaf 7 + XCR0 ZMM | Skylake-X+ (2016), max power, freq throttling |

## Power Characteristics

Different SIMD levels have different power/thermal impacts:

- **SSE**: Baseline power consumption, no frequency throttling
- **AVX**: Moderate power increase, minimal throttling on modern CPUs
- **AVX2**: Higher power (FMA), may trigger "AVX offset" on some CPUs
- **AVX-512**: Maximum power draw, often triggers frequency throttling (AVX-512 "tax")

## Testing P-States with SIMD Levels

Use CoreBurner to validate CPU power states (P0, P1, etc.) under different instruction set loads:

```bash
# Test P0 (max performance) with AVX-512
sudo ./coreburner --mode multi --util 100 --duration 60s \
  --type AVX512 --set-governor performance --log p0_avx512.csv

# Test P1 (reduced frequency) with AVX2
sudo ./coreburner --mode multi --util 75 --duration 60s \
  --type AVX2 --set-max-freq 2400000 --log p1_avx2.csv

# Compare power at different SIMD levels
for simd in SSE AVX AVX2 AVX512; do
  ./coreburner --mode multi --util 90 --duration 30s \
    --type $simd --log ${simd}_power_test.csv
done
```

## Frequency Throttling Detection

AVX-512 workloads often trigger frequency throttling. Monitor with:

```bash
# Watch frequency during AVX-512 load
./coreburner --mode multi --util 100 --duration 2m \
  --type AVX512 --log avx512_freq.csv

# Compare to AVX2 (should maintain higher frequency)
./coreburner --mode multi --util 100 --duration 2m \
  --type AVX2 --log avx2_freq.csv

# Check the CSV logs - frequency columns show per-core frequencies
```

## Feature Detection

CoreBurner automatically detects CPU capabilities:

```bash
# This will fail gracefully if AVX-512 not supported
./coreburner --mode single --util 50 --duration 10s --type AVX512 --check
# Output: "Error: CPU or OS does not support AVX-512F."

# Check what's supported on your system
./coreburner --mode single --util 50 --duration 10s --type SSE --check  # Should work
./coreburner --mode single --util 50 --duration 10s --type AVX2 --check # Check AVX2
```

## MIXED Mode

MIXED mode uses AVX2 by default for the SIMD portion. Configure ratios with `--mixed-ratio`:

```bash
# 50% INT, 30% FLOAT, 20% AVX2
./coreburner --mode multi --util 80 --duration 1m \
  --type MIXED --mixed-ratio 5:3:2 --log mixed_test.csv
```

## Compilation Notes

To enable AVX-512 at compile time (optional, runtime fallback exists):

```bash
gcc -O2 -march=skylake-avx512 -pthread -std=c11 \
    -Wall -Wextra -o coreburner coreburner.c -lm
```

Or use `-march=native` to enable all features your build CPU supports.

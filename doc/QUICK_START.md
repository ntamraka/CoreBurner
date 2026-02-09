# CoreBurner DCL Validation - Quick Start Guide

## What is This?

CoreBurner is a CPU frequency validation tool that helps verify Intel processors meet their DCL (Data Center List) specifications. It's designed for:

- **Intel validation engineers** testing custom SKUs
- **ODM/OEM partners** validating server configurations
- **Customers** verifying CPU performance specs

## Installation

```bash
# Clone or download CoreBurner
cd CoreBurner

# Compile
make

# Optional: Install system-wide
sudo make install
```

## Quick Test (30 seconds)

Test if your SSE workload hits the expected frequency:

```bash
# Run SSE workload and check frequency
# Replace 3200 with your expected MHz
./coreburner --mode single --util 100 --duration 10s --type SSE \
  --dcl-sse-freq 3200 --dcl-tolerance 3.0
```

**Look for this output:**
```
=== DCL Frequency Validation ===
  Workload Type     : SSE P0n
  Expected Frequency: 3200 MHz
  Measured Frequency: 3180 MHz
  Deviation         : 0.62%
  Tolerance         : 3.00%
  Result            : PASS
================================
```

## Full Validation Suite (5 minutes)

Run automated tests across all workload types:

```bash
# 1. Edit the script to set your DCL specifications
nano script/validate_dcl_frequencies.sh

# Update these lines:
#   DCL_SSE_P0N=3200
#   DCL_AVX_P0N=3000
#   DCL_AVX2_P0N=2800

# 2. Run the validation suite
sudo ./script/validate_dcl_frequencies.sh

# 3. Check results
# Results will be in: ./dcl_validation_TIMESTAMP/
```

## Understanding Results

### Cdyn Class
Shows which power/frequency class the workload belongs to:
- **Cdyn0 (Low)**: SSE, Integer, Float operations
- **Cdyn1 (Medium)**: AVX 256-bit operations
- **Cdyn2 (High)**: AVX2, AVX-512 operations

### Validation Status
- **PASS**: Measured frequency within tolerance of expected
- **FAIL**: Frequency deviation exceeds tolerance

## Common Test Scenarios

### 1. P0n Test (Single Core Max Frequency)
```bash
./coreburner --mode single --util 100 --duration 60s --type AVX2 \
  --dcl-avx2-freq 2800 --dcl-tolerance 3.0
```

### 2. All-Core Turbo Test
```bash
./coreburner --mode multi --util 100 --duration 60s --type SSE \
  --dcl-sse-freq 2900 --dcl-tolerance 3.0
```

### 3. Partial Load Test (<100% Utilization)
```bash
./coreburner --mode multi --util 75 --duration 60s --type AVX \
  --dcl-avx-freq 2700 --dcl-tolerance 5.0
```

## Troubleshooting

### "Result: FAIL" - Frequency Too Low

**Possible causes:**
1. **Thermal throttling** - Check temperatures (shown in output)
2. **Power limits** - CPU may be power-limited
3. **CPU Governor** - Try setting to performance mode:
   ```bash
   sudo cpupower frequency-set -g performance
   ```

### "Permission denied" for MSR

MSR access requires root and the msr kernel module:
```bash
sudo modprobe msr
sudo ./coreburner --enable-msr-freq ...
```

### Frequency Much Higher/Lower Than Expected

1. Check you're using the correct mode (single vs multi)
2. Verify DCL specs match your CPU model
3. Check BIOS settings (Turbo enabled, power limits)

## What to Include in Bug Reports

If you encounter issues:

```bash
# 1. System information
uname -a
lscpu
cat /proc/cpuinfo | grep "model name" | head -1

# 2. CoreBurner version
./coreburner --help | head -1

# 3. Test command used
# (copy your full command)

# 4. Output
# (attach the CSV log and summary files)

# 5. CPU frequency info
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
```

## Next Steps

- **For detailed documentation**: See [DCL_VALIDATION.md](DCL_VALIDATION.md)
- **For advanced features**: Check `./coreburner --help`
- **For automation**: See `validate_dcl_frequencies.sh` script
- **For understanding the problem**: Read [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md)

## Support

- GitHub Issues: https://github.com/ntamraka/CoreBurner/issues
- Email: [Your contact]

---

**Remember**: For accurate results:
- Run on idle system (no other heavy workloads)
- Use root access when possible
- Set CPU governor to "performance"
- Monitor temperatures
- Run multiple samples and average results

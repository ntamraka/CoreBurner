# DCL Frequency Validation with CoreBurner

## Overview

CoreBurner now includes comprehensive DCL (Data Center List) frequency validation capabilities, designed to address Intel's CWF (Custom Workload Frequency) validation requirements as discussed in internal communications regarding ByteDance and other customer engagements.

## Problem Statement

### Background
Modern Intel processors control frequency based on **Cdyn Class** (dynamic capacitance) and electrical factors, not just instruction types. This creates challenges for:

1. **Specification**: DCL traditionally specified "SSE P0n" or "AVX2 P0n" frequencies, but actual behavior depends on Cdyn class
2. **Customer Validation**: Customers need tools to verify that actual CPU frequencies match DCL specifications
3. **Communication**: Need alignment between CPU customization, DCL documentation, and validation tools

### CoreBurner Solution

CoreBurner bridges this gap by:
- Generating workloads with specific instruction types (SSE, AVX, AVX2, AVX512)
- Measuring actual CPU frequencies during execution
- Mapping workloads to Cdyn classes
- Validating measured frequencies against DCL specifications
- Providing detailed reports with pass/fail criteria

---

## Features

### 1. Cdyn Class Detection

CoreBurner automatically classifies workloads into Cdyn classes:

| Cdyn Class | Dynamic Capacitance | Workload Types |
|------------|--------------------|-----------------| 
| **Cdyn0 (Low)** | Low power | INT, FLOAT, SSE |
| **Cdyn1 (Medium)** | Medium power | AVX |
| **Cdyn2 (High)** | High power | AVX2, AVX512 |

### 2. Frequency Measurement

- **Sysfs Method** (default): Reads `scaling_cur_freq` from cpufreq sysfs
- **MSR Method** (optional): Uses APERF/MPERF counters for precise measurement (requires MSR access)

### 3. Power Monitoring

- **RAPL Support**: Measures package, core, and DRAM power via MSR registers
- Useful for correlating power consumption with frequency behavior

### 4. Frequency Residency Tracking

- Tracks time spent at different frequency states
- Useful for UAT (Utilization At Turbo) high/med/low validation
- Generates histograms showing frequency distribution

### 5. DCL Validation

- Compare measured frequencies against expected DCL values
- Configurable tolerance percentage
- Clear PASS/FAIL reporting
- Per-workload-type validation (SSE P0n, AVX P0n, AVX2 P0n, etc.)

---

## Usage

### Basic Frequency Validation

Validate that SSE workload achieves expected P0n frequency:

```bash
sudo ./coreburner \
  --mode multi \
  --util 100 \
  --duration 60s \
  --type SSE \
  --dcl-sse-freq 3200 \
  --dcl-tolerance 3.0 \
  --log sse_validation.csv
```

**Output:**
```
=== Cdyn Class Analysis ===
  Workload Type: SSE
  Cdyn Class   : Cdyn0 (Low)
===========================

=== DCL Frequency Validation ===
  Workload Type     : SSE P0n
  Expected Frequency: 3200 MHz
  Measured Frequency: 3180 MHz
  Deviation         : 0.62%
  Tolerance         : 3.00%
  Result            : PASS
================================
```

### Multi-Workload Validation

Test multiple instruction types with their respective DCL frequencies:

```bash
#!/bin/bash
# Test SSE (Cdyn0)
./coreburner --mode multi --util 100 --duration 30s --type SSE \
  --dcl-sse-freq 3200 --dcl-tolerance 3.0 --log sse_dcl.csv

# Test AVX (Cdyn1)  
./coreburner --mode multi --util 100 --duration 30s --type AVX \
  --dcl-avx-freq 3000 --dcl-tolerance 3.0 --log avx_dcl.csv

# Test AVX2 (Cdyn2)
./coreburner --mode multi --util 100 --duration 30s --type AVX2 \
  --dcl-avx2-freq 2800 --dcl-tolerance 3.0 --log avx2_dcl.csv
```

### Advanced: MSR-Based Frequency Measurement

For more precise frequency measurement using APERF/MPERF counters:

```bash
# Requires MSR kernel module and root access
sudo modprobe msr

sudo ./coreburner \
  --mode multi \
  --util 100 \
  --duration 60s \
  --type AVX2 \
  --dcl-avx2-freq 2800 \
  --enable-msr-freq \
  --base-freq 2000 \
  --log avx2_msr_validation.csv
```

### Power Monitoring with RAPL

Measure power consumption alongside frequency validation:

```bash
sudo ./coreburner \
  --mode multi \
  --util 100 \
  --duration 120s \
  --type AVX512 \
  --dcl-avx512-freq 2600 \
  --enable-rapl \
  --enable-msr-freq \
  --log avx512_power_freq.csv
```

---

## Command-Line Options

### DCL Validation Options

| Option | Description | Default |
|--------|-------------|---------|
| `--validate-dcl` | Enable DCL frequency validation | Disabled |
| `--dcl-sse-freq MHZ` | Expected SSE P0n frequency | - |
| `--dcl-avx-freq MHZ` | Expected AVX P0n frequency | - |
| `--dcl-avx2-freq MHZ` | Expected AVX2 P0n frequency | - |
| `--dcl-avx512-freq MHZ` | Expected AVX512 P0n frequency | - |
| `--dcl-tolerance PCT` | Allowed deviation percentage | 3.0 |
| `--enable-msr-freq` | Use MSR APERF/MPERF for frequency | Disabled |
| `--enable-rapl` | Enable RAPL power monitoring | Disabled |
| `--base-freq MHZ` | Base frequency for APERF/MPERF calc | 2000 |

---

## Requirements

### For Basic Validation (sysfs-based)
- Linux kernel with cpufreq support
- Read access to `/sys/devices/system/cpu/*/cpufreq/scaling_cur_freq`

### For MSR-Based Measurements
```bash
# Load MSR kernel module
sudo modprobe msr

# Verify access
ls -l /dev/cpu/0/msr
```

### For RAPL Power Monitoring
- Intel processor with RAPL support (Sandy Bridge or newer)
- MSR kernel module loaded
- Root access to read MSR registers

---

## Typical Validation Workflow

### 1. Define DCL Specifications

Create a test specification based on your DCL:

```bash
# Example for CWF SKU
DCL_SSE_P0N=3200      # MHz
DCL_AVX_P0N=3000      # MHz
DCL_AVX2_P0N=2800     # MHz
DCL_TOLERANCE=3.0     # Percent
```

### 2. Run Validation Tests

```bash
#!/bin/bash
# validation_suite.sh

LOG_DIR="./dcl_validation_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$LOG_DIR"

echo "Starting DCL Frequency Validation Suite..."

# P0n (100% single-core load)
./coreburner --mode single --util 100 --duration 60s --type SSE \
  --dcl-sse-freq $DCL_SSE_P0N --dcl-tolerance $DCL_TOLERANCE \
  --log "$LOG_DIR/sse_p0n.csv"

./coreburner --mode single --util 100 --duration 60s --type AVX \
  --dcl-avx-freq $DCL_AVX_P0N --dcl-tolerance $DCL_TOLERANCE \
  --log "$LOG_DIR/avx_p0n.csv"

./coreburner --mode single --util 100 --duration 60s --type AVX2 \
  --dcl-avx2-freq $DCL_AVX2_P0N --dcl-tolerance $DCL_TOLERANCE \
  --log "$LOG_DIR/avx2_p0n.csv"

# All-Core Turbo (100% multi-core load)
./coreburner --mode multi --util 100 --duration 60s --type SSE \
  --dcl-sse-freq 2900 --dcl-tolerance $DCL_TOLERANCE \
  --log "$LOG_DIR/sse_allcore.csv"

echo "Validation complete. Results in $LOG_DIR/"
```

### 3. Analyze Results

Review the CSV logs and summary files:

```bash
# Check summary files
cat $LOG_DIR/*.summary.txt

# Extract validation results
grep "Result" $LOG_DIR/*.csv
```

---

## Understanding Output

### Cdyn Class Report

```
=== Cdyn Class Analysis ===
  Workload Type: AVX2
  Cdyn Class   : Cdyn2 (High)
===========================
```

This shows:
- **Workload Type**: The instruction set level being tested
- **Cdyn Class**: The dynamic capacitance class (affects frequency and power)

### DCL Validation Report

```
=== DCL Frequency Validation ===
  Workload Type     : AVX2 P0n
  Expected Frequency: 2800 MHz
  Measured Frequency: 2785 MHz
  Deviation         : 0.54%
  Tolerance         : 3.00%
  Result            : PASS
================================
```

This shows:
- **Workload Type**: Specific test case (e.g., "AVX2 P0n")
- **Expected Frequency**: Value from DCL specification
- **Measured Frequency**: Actual average frequency during test
- **Deviation**: Percentage difference
- **Tolerance**: Acceptable deviation
- **Result**: PASS if deviation ≤ tolerance, FAIL otherwise

---

## Troubleshooting

### Issue: FAIL with High Deviation

**Symptom:**
```
Deviation         : 25.00%
Result            : FAIL
```

**Possible Causes:**
1. **Insufficient Load**: Use `--util 100` and `--mode multi` for max frequency
2. **Thermal Throttling**: Check temperature, improve cooling
3. **Power Limits**: Check RAPL limits, adjust TDP settings
4. **Governor Settings**: Set to "performance" mode
   ```bash
   sudo cpupower frequency-set -g performance
   ```

### Issue: MSR Access Denied

**Symptom:**
```
Error opening /dev/cpu/0/msr: Permission denied
```

**Solution:**
```bash
# Load MSR module
sudo modprobe msr

# Run with root
sudo ./coreburner --enable-msr-freq ...
```

### Issue: Low Measured Frequency

**Symptom:** Frequency much lower than expected

**Checks:**
1. CPU governor: `cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor`
2. Frequency limits: `cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq`
3. Temperature: `sensors` or check CoreBurner temperature output
4. Power policy: `cat /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference`

---

## Best Practices

### 1. Stabilization Period
Allow CPU to reach steady-state before validation:
```bash
# Warm-up run
./coreburner --mode multi --util 100 --duration 30s --type AVX2 --log /dev/null

# Then validation run
./coreburner --mode multi --util 100 --duration 120s --type AVX2 \
  --dcl-avx2-freq 2800 --log avx2_validation.csv
```

### 2. Multiple Samples
Run tests multiple times and average results:
```bash
for i in {1..5}; do
  ./coreburner --mode multi --util 100 --duration 60s --type SSE \
    --dcl-sse-freq 3200 --log sse_run_$i.csv
done
```

### 3. Controlled Environment
- Disable power-saving features
- Set fixed frequencies if testing specific points
- Monitor system load (no interference from other processes)

### 4. Documentation
- Record system configuration (BIOS settings, kernel version)
- Save DCL specifications alongside validation results
- Include temperature and power measurements in reports

---

## Integration with Intel Validation Workflow

CoreBurner can be integrated into existing Intel validation processes:

### For Customer Validation
1. Provide CoreBurner binary + test scripts to customers
2. Customer runs validation suite on their hardware
3. Results CSV files sent back for analysis
4. Automated pass/fail reporting

### For Internal Pre-shipment Testing
1. Automated testing in validation lab
2. Integration with existing test frameworks
3. Continuous monitoring of frequency behavior
4. Regression detection across BIOS/microcode updates

### For DCL Specification
1. Use CoreBurner to measure actual frequencies during design
2. Update DCL specs based on measured data
3. Validate spec accuracy before customer disclosure

---

## Mapping to Email Discussion

This implementation addresses the concerns raised in the Intel internal discussion:

| Concern | CoreBurner Solution |
|---------|-------------------|
| **Cdyn Class vs Instruction Type** | Automatic mapping + clear reporting |
| **Customer Measurement** | Standard tool + CSV output |
| **P0n vs All-Core Turbo** | Flexible --mode single/multi |
| **<100% Load TRL** | --util parameter for any load level |
| **UAT High/Med/Low** | Frequency residency tracking |
| **Validation Method** | Built-in DCL comparison with tolerance |
| **Tool Development** | Complete, extensible, open for customization |

---

## Future Enhancements

Planned features for future versions:

1. **Config File Support**: JSON/INI file for DCL specifications
2. **Automated Report Generation**: HTML/PDF reports
3. **Historical Tracking**: Database of validation results over time
4. **Advanced Analytics**: Statistical analysis, trend detection
5. **Multi-node Testing**: Distributed validation across multiple servers

---

## Support

For issues, enhancements, or questions:
- GitHub Issues: [CoreBurner Repository](https://github.com/ntamraka/CoreBurner)
- Internal Intel Contact: [Your contact information]

---

*Document Version: 1.0*  
*Date: December 19, 2025*  
*CoreBurner Version: 2.0+dcl*

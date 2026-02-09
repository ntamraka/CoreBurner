# CoreBurner Docker Container Guide

This guide explains how to run CoreBurner in a Docker container for CPU stress testing, telemetry, and frequency validation.

## Quick Start

### Build the Docker Image

```bash
# Build the image
docker build -t coreburner:latest .

# Or using docker-compose
docker-compose build
```

### Run Basic Stress Test

```bash
# Run default stress test (75% utilization, AVX2, 60 seconds)
docker-compose up coreburner

# View logs in ./log/stress_test.csv
```

## Docker Architecture

The Dockerfile uses a **multi-stage build**:
1. **Builder stage**: Compiles CoreBurner from source using GCC
2. **Runtime stage**: Creates a minimal Debian-based image with only necessary dependencies

### Key Features:
- **Privileged mode**: Required for CPU frequency control and accurate monitoring
- **Host mounts**: `/proc` and `/sys` for CPU telemetry
- **Volume mounts**: Persistent log storage
- **Host networking**: Better performance monitoring accuracy

## Running CoreBurner Container

### Method 1: Docker Compose (Recommended)

#### Default Service (Non-root)
```bash
# Run with default settings
docker-compose up coreburner

# Run in detached mode
docker-compose up -d coreburner

# Override command
docker-compose run --rm coreburner \
  --mode multi --util 90 --duration 2m --type AVX --log /app/log/test.csv
```

#### Interactive Mode
```bash
# Start interactive shell
docker-compose --profile interactive up -d coreburner-interactive

# Execute commands inside container
docker exec -it coreburner-interactive /app/coreburner --check-simd
docker exec -it coreburner-interactive /app/coreburner --mode single --util 50 --duration 30s --type AVX2

# Get shell access
docker exec -it coreburner-interactive /bin/bash

# Stop interactive container
docker-compose --profile interactive down
```

#### Root Mode (For Frequency Control)
```bash
# Run with root privileges for governor/frequency control
docker-compose --profile root up coreburner-root

# Custom root command
docker-compose --profile root run --rm coreburner-root \
  --mode multi --util 100 --duration 3m --type AVX2 \
  --set-governor performance --set-max-freq 3400000 \
  --temp-threshold 85 --dynamic-freq \
  --log /app/log/root_test.csv
```

### Method 2: Direct Docker Commands

#### Basic Run
```bash
docker run --rm --privileged \
  -v /proc:/host/proc:ro \
  -v /sys:/host/sys:ro \
  -v $(pwd)/log:/app/log \
  --network host \
  coreburner:latest \
  --mode multi --util 75 --duration 60s --type AVX2 --log /app/log/test.csv
```

#### With Root Access
```bash
docker run --rm --privileged --user root \
  -v /proc:/host/proc:ro \
  -v /sys:/host/sys \
  -v $(pwd)/log:/app/log \
  --network host \
  coreburner:latest \
  --mode multi --util 100 --duration 2m --type AVX2 \
  --set-governor performance --temp-threshold 85 \
  --log /app/log/test.csv
```

#### Interactive Shell
```bash
docker run -it --rm --privileged \
  -v /proc:/host/proc:ro \
  -v /sys:/host/sys:ro \
  -v $(pwd)/log:/app/log \
  --network host \
  --entrypoint /bin/bash \
  coreburner:latest
```

## Common Use Cases

### 1. SIMD Instruction Set Testing

```bash
# Test SSE (128-bit)
docker-compose run --rm coreburner \
  --mode single --util 95 --duration 30s --type SSE --log /app/log/sse_test.csv

# Test AVX (256-bit)
docker-compose run --rm coreburner \
  --mode single --util 95 --duration 30s --type AVX --log /app/log/avx_test.csv

# Test AVX2 (256-bit with FMA)
docker-compose run --rm coreburner \
  --mode single --util 95 --duration 30s --type AVX2 --log /app/log/avx2_test.csv

# Test AVX-512 (512-bit)
docker-compose run --rm coreburner \
  --mode single --util 95 --duration 30s --type AVX512 --log /app/log/avx512_test.csv
```

### 2. Multi-Core Stress Testing

```bash
# Full system load
docker-compose run --rm coreburner \
  --mode multi --util 100 --duration 5m --type AVX2 --log /app/log/full_load.csv

# Partial load (50%)
docker-compose run --rm coreburner \
  --mode multi --util 50 --duration 10m --type MIXED --mixed-ratio 3:2:1 --log /app/log/partial_load.csv
```

### 3. DCL Frequency Validation

```bash
# Validate AVX2 P0n frequency
docker-compose run --rm coreburner \
  --mode multi --util 100 --duration 60s --type AVX2 \
  --dcl-avx2-freq 2800 --dcl-tolerance 3.0 \
  --log /app/log/dcl_validation.csv
```

### 4. Thermal Testing with Dynamic Frequency Tuning

```bash
# Run with automatic frequency reduction on overheat
docker-compose --profile root run --rm coreburner-root \
  --mode multi --util 100 --duration 10m --type AVX2 \
  --dynamic-freq --temp-threshold 85 \
  --log /app/log/thermal_test.csv
```

### 5. Single Core Multi-Thread (SMT/Hyperthreading) Testing

```bash
# Test 4 threads on core 0
docker-compose run --rm coreburner \
  --mode single-core-multi --single-core-id 0 --single-core-threads 4 \
  --util 90 --duration 2m --type MIXED --mixed-ratio 3:2:1 \
  --log /app/log/smt_test.csv
```

## Volume Mounts

The docker-compose.yml sets up these mounts:

| Host Path | Container Path | Purpose | Mode |
|-----------|----------------|---------|------|
| `/proc` | `/host/proc` | CPU utilization monitoring | Read-only |
| `/sys` | `/host/sys` | CPU frequency & temperature | Read-only (RW for root) |
| `./log` | `/app/log` | Persistent log storage | Read-write |
| `./results` | `/app/results` | Test results | Read-write |

## Viewing Results

```bash
# View real-time logs
tail -f log/stress_test.csv

# View summary
cat log/stress_test.csv.summary.txt

# View all logs
ls -lh log/
```

## Resource Limits (Optional)

To limit container resources, uncomment and modify in docker-compose.yml:

```yaml
deploy:
  resources:
    limits:
      cpus: '4'      # Limit to 4 CPU cores
      memory: 2G     # Limit to 2GB RAM
```

## Troubleshooting

### Permission Errors

If you get permission errors accessing `/sys/devices/system/cpu/`:
- Run with `--profile root` or `--user root`
- Ensure `privileged: true` is set

### Container Can't See All CPUs

Make sure:
- `--network host` is used
- `privileged: true` is set
- No strict cgroup CPU limits are applied

### Temperature Monitoring Not Working

- Ensure `/sys` is mounted correctly
- Some systems may not expose temperature sensors in the same way
- Try running with root privileges

### Logs Not Persisting

- Ensure `./log` directory exists on host
- Check volume mount permissions
- Logs are written to `/app/log` inside container

## Security Considerations

⚠️ **Important**: CoreBurner requires privileged mode for:
- CPU frequency manipulation
- Direct hardware access
- Temperature monitoring
- Accurate performance counters

Only run in trusted environments. For production monitoring, consider:
- Running as non-root user (limited functionality)
- Using read-only mounts where possible
- Restricting network access
- Using resource limits

## Building for Different Architectures

```bash
# Build for ARM64
docker build --platform linux/arm64 -t coreburner:arm64 .

# Build for AMD64
docker build --platform linux/amd64 -t coreburner:amd64 .

# Multi-arch build
docker buildx build --platform linux/amd64,linux/arm64 -t coreburner:multi .
```

## CI/CD Integration

Example GitHub Actions workflow:

```yaml
name: Build and Test CoreBurner

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build Docker image
        run: docker build -t coreburner:test .
      - name: Run basic test
        run: |
          docker run --rm --privileged \
            -v /proc:/host/proc:ro \
            -v /sys:/host/sys:ro \
            coreburner:test \
            --mode single --util 50 --duration 10s --type INT
```

## Advanced Configuration

### Custom Dockerfile Modifications

To add custom tools or dependencies:

```dockerfile
# Add after the RUN apt-get install line
RUN apt-get update && apt-get install -y \
    your-custom-package \
    && rm -rf /var/lib/apt/lists/*
```

### Environment Variables

Set environment variables in docker-compose.yml:

```yaml
environment:
  - COREBURNER_DEFAULT_UTIL=75
  - COREBURNER_LOG_DIR=/app/log
```

## Cleaning Up

```bash
# Remove containers
docker-compose down

# Remove images
docker rmi coreburner:latest

# Clean all Docker resources
docker system prune -a
```

## Support

For more information:
- See [README.md](README.md) for CoreBurner usage
- See [DCL_VALIDATION.md](DCL_VALIDATION.md) for frequency validation
- See [QUICK_START.md](QUICK_START.md) for basic examples


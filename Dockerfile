# Multi-stage build for CoreBurner CPU Stress Testing Tool
FROM gcc:13-bookworm AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    make \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /build

# Copy source files
COPY coreburner.c Makefile ./

# Build the application
RUN make clean && make

# Runtime stage - minimal image
FROM debian:bookworm-slim

# Install runtime dependencies and tools for CPU monitoring
RUN apt-get update && apt-get install -y \
    procps \
    util-linux \
    lm-sensors \
    cpufrequtils \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user (some operations will still require root)
RUN useradd -m -u 1000 -s /bin/bash coreburner

# Set working directory
WORKDIR /app

# Copy compiled binary from builder
COPY --from=builder /build/coreburner /app/coreburner

# Copy scripts and documentation
COPY script/ /app/script/
COPY doc/ /app/doc/
COPY *.md ./
RUN chmod +x /app/script/*.sh coreburner

# Create log directory
RUN mkdir -p /app/log && chown -R coreburner:coreburner /app

# Switch to non-root user (can be overridden with --user root)
USER coreburner

# Default command - show help
ENTRYPOINT ["/app/coreburner"]
CMD ["--help"]

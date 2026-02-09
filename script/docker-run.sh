#!/bin/bash
# CoreBurner Docker Quick Run Script

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}CoreBurner Docker Quick Run Script${NC}"
echo "======================================="
echo ""

# Check if Docker is installed
if ! command -v docker &> /dev/null; then
    echo -e "${RED}Error: Docker is not installed${NC}"
    exit 1
fi

# Check if docker-compose is available
if ! command -v docker-compose &> /dev/null; then
    echo -e "${YELLOW}Warning: docker-compose not found. Using 'docker compose' instead${NC}"
    COMPOSE_CMD="docker compose"
else
    COMPOSE_CMD="docker-compose"
fi

# Function to show usage
show_usage() {
    echo "Usage: $0 [command]"
    echo ""
    echo "Commands:"
    echo "  build           Build the Docker image"
    echo "  run             Run default stress test (75% util, AVX2, 60s)"
    echo "  interactive     Start interactive container"
    echo "  root            Run with root privileges"
    echo "  test-simd       Test all SIMD instruction sets"
    echo "  help            Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 build              # Build the image"
    echo "  $0 run                # Run default test"
    echo "  $0 interactive        # Start interactive shell"
    echo ""
}

# Parse command
case "${1:-help}" in
    build)
        echo -e "${GREEN}Building CoreBurner Docker image...${NC}"
        docker build -t coreburner:latest .
        echo -e "${GREEN}Build complete!${NC}"
        ;;
    
    run)
        echo -e "${GREEN}Running default stress test...${NC}"
        mkdir -p log results
        $COMPOSE_CMD up coreburner
        echo ""
        echo -e "${GREEN}Test complete! Check log/stress_test.csv for results${NC}"
        ;;
    
    interactive)
        echo -e "${GREEN}Starting interactive container...${NC}"
        mkdir -p log results
        $COMPOSE_CMD --profile interactive up -d coreburner-interactive
        echo ""
        echo -e "${GREEN}Container started. Access it with:${NC}"
        echo "  docker exec -it coreburner-interactive /bin/bash"
        echo ""
        echo -e "${YELLOW}To stop: $COMPOSE_CMD --profile interactive down${NC}"
        ;;
    
    root)
        echo -e "${GREEN}Running with root privileges...${NC}"
        mkdir -p log results
        $COMPOSE_CMD --profile root up coreburner-root
        echo ""
        echo -e "${GREEN}Test complete! Check log/stress_root.csv for results${NC}"
        ;;
    
    test-simd)
        echo -e "${GREEN}Testing all SIMD instruction sets...${NC}"
        mkdir -p log results
        
        echo -e "${YELLOW}Testing SSE (128-bit)...${NC}"
        $COMPOSE_CMD run --rm coreburner --mode single --util 95 --duration 10s --type SSE --log /app/log/sse_test.csv
        
        echo -e "${YELLOW}Testing AVX (256-bit)...${NC}"
        $COMPOSE_CMD run --rm coreburner --mode single --util 95 --duration 10s --type AVX --log /app/log/avx_test.csv
        
        echo -e "${YELLOW}Testing AVX2 (256-bit + FMA)...${NC}"
        $COMPOSE_CMD run --rm coreburner --mode single --util 95 --duration 10s --type AVX2 --log /app/log/avx2_test.csv
        
        echo ""
        echo -e "${GREEN}SIMD tests complete! Results in log/ directory${NC}"
        ;;
    
    help|--help|-h)
        show_usage
        ;;
    
    *)
        echo -e "${RED}Unknown command: $1${NC}"
        echo ""
        show_usage
        exit 1
        ;;
esac

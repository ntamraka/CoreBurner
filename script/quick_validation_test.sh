#!/bin/bash
echo "=========================================="
echo "CoreBurner DCL Validation - Quick Test"
echo "=========================================="
echo ""

echo "Test 1: SSE (Cdyn0 - Low Power)"
../coreburner --mode single --util 100 --duration 5s --type SSE \
  --dcl-sse-freq 2800 --dcl-tolerance 10.0 --log test_sse.csv 2>&1 | \
  grep -A 10 "=== Cdyn Class Analysis ===" 

echo ""
echo "Test 2: AVX (Cdyn1 - Medium Power)"
../coreburner --mode single --util 100 --duration 5s --type AVX \
  --dcl-avx-freq 2600 --dcl-tolerance 10.0 --log test_avx.csv 2>&1 | \
  grep -A 10 "=== Cdyn Class Analysis ==="

echo ""
echo "Test 3: AVX2 All-Core (Cdyn2 - High Power)"  
../coreburner --mode multi --util 100 --duration 5s --type AVX2 \
  --dcl-avx2-freq 2500 --dcl-tolerance 10.0 --log test_avx2.csv 2>&1 | \
  grep -A 10 "=== Cdyn Class Analysis ==="

echo ""
echo "=========================================="
echo "Validation Tests Complete!"
echo "=========================================="

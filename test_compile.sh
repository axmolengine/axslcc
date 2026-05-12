#!/bin/bash

AXSLCC=./build/axslcc
INPUT_DIR=/Users/simdsoft/oss/axmol/axmol/renderer/shaders
OUTPUT_DIR=tmp/test_output
FAILED=0
SUCCESS=0

mkdir -p "$OUTPUT_DIR"

echo "Testing GLSL to multiple targets compilation..."

# Test scenario 1: Fragment shaders
for frag in "$INPUT_DIR"/*.frag; do
    filename=$(basename "$frag")
    stem="${filename%.*}"
    
    # Test with -sc and -reflect
    if $AXSLCC --input="$frag" --output="$OUTPUT_DIR/$stem" --target=hlsl-51\;glsl-450 --sc --reflect 2>/dev/null; then
        echo "✓ $filename (sc + reflect)"
        ((SUCCESS++))
    else
        echo "✗ $filename (sc + reflect)"
        ((FAILED++))
    fi
done

# Test scenario 2: Vertex shaders
for vert in "$INPUT_DIR"/*.vert; do
    filename=$(basename "$vert")
    stem="${filename%.*}"
    
    # Test with -sc
    if $AXSLCC --input="$vert" --output="$OUTPUT_DIR/$stem" --target=hlsl-51\;glsl-330 --sc 2>/dev/null; then
        echo "✓ $filename (sc)"
        ((SUCCESS++))
    else
        echo "✗ $filename (sc)"
        ((FAILED++))
    fi
done

echo ""
echo "Results: $SUCCESS passed, $FAILED failed"

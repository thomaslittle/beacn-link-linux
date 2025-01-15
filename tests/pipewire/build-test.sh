#!/bin/bash

# Exit on error
set -e

# Test results
declare -A test_results
test_results["basic"]="Not Run"
test_results["stream"]="Not Run"
test_results["control"]="Not Run"
test_results["multi"]="Not Run"
test_results["stress"]="Not Run"

# Cleanup function
cleanup() {
    echo -e "\nCleaning up..."
    # Kill any remaining test processes
    pkill -f "test_(pipewire|stream|controls|multi_stream|stress)" 2>/dev/null || true
    # Remove test binaries
    rm -f test_pipewire test_stream test_controls test_multi_stream test_stress
    
    # Print test summary
    echo -e "\nTest Summary:"
    echo "----------------------------------------"
    echo "Basic PipeWire Test:    ${test_results[basic]}"
    echo "Stream Test:            ${test_results[stream]}"
    echo "Control Test:           ${test_results[control]}"
    echo "Multi-Stream Test:      ${test_results[multi]}"
    echo "Stress Test:            ${test_results[stress]}"
    echo "----------------------------------------"
    
    exit 0
}

# Handle interrupts
trap cleanup SIGINT SIGTERM

echo "Building PipeWire tests..."

# Compile basic test
echo "Building basic connectivity test..."
gcc -o test_pipewire test_pipewire.c $(pkg-config --cflags --libs libpipewire-0.3) -Wall -Wextra

echo "Building stream test..."
gcc -o test_stream test_stream.c $(pkg-config --cflags --libs libpipewire-0.3) -Wall -Wextra -lm

echo "Building control test..."
gcc -o test_controls test_controls.c $(pkg-config --cflags --libs libpipewire-0.3) -Wall -Wextra -lm

echo "Building multi-stream test..."
gcc -o test_multi_stream test_multi_stream.c $(pkg-config --cflags --libs libpipewire-0.3) -Wall -Wextra -lm

echo "Building stress test..."
gcc -o test_stress test_stress.c $(pkg-config --cflags --libs libpipewire-0.3) -Wall -Wextra

echo -e "\nRunning tests..."

echo "1. Running basic PipeWire test..."
if ./test_pipewire; then
    test_results["basic"]="PASSED"
else
    test_results["basic"]="FAILED"
    cleanup
    exit 1
fi

echo -e "\n2. Running stream test..."
if timeout 5s ./test_stream; then
    test_results["stream"]="PASSED"
else
    if [ $? -eq 124 ]; then
        echo "Stream test timed out"
        test_results["stream"]="TIMEOUT"
    else
        test_results["stream"]="FAILED"
    fi
    cleanup
    exit 1
fi

echo -e "\n3. Running control test..."
if timeout 10s ./test_controls; then
    test_results["control"]="PASSED"
else
    if [ $? -eq 124 ]; then
        echo "Control test timed out"
        test_results["control"]="TIMEOUT"
    else
        test_results["control"]="FAILED"
    fi
    cleanup
    exit 1
fi

echo -e "\n4. Running multi-stream test..."
if timeout 20s ./test_multi_stream; then
    test_results["multi"]="PASSED"
else
    if [ $? -eq 124 ]; then
        echo "Multi-stream test timed out"
        test_results["multi"]="TIMEOUT"
    else
        test_results["multi"]="FAILED"
    fi
    cleanup
    exit 1
fi

echo -e "\n5. Running stress test..."
if timeout 35s ./test_stress; then
    test_results["stress"]="PASSED"
else
    if [ $? -eq 124 ]; then
        echo "Stress test timed out"
        test_results["stress"]="TIMEOUT"
    else
        test_results["stress"]="FAILED"
    fi
    cleanup
    exit 1
fi

# Final cleanup and summary
cleanup 

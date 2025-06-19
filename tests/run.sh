#!/bin/bash
TEST="$1"
test -z "$TEST" && { echo "Usage: $0 <test_name>"; exit 1; }
ITER=10000
echo "Running test: $TEST for $ITER iterations"

make all

for ((i=1;i<=ITER;i++)); do
    echo "$TEST Running iteration $i of $ITER"
    LD_LIBRARY_PATH=.. ./$TEST > /dev/null
    if [ $? -ne 0 ]; then
        echo "Error encountered in iteration $i, exiting."
        exit 1
    fi
done
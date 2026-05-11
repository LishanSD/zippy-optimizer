#!/bin/bash

# Create results directory if it doesn't exist
mkdir -p results

echo "===================================================="
echo " Starting VLDB-Scale Zippy Optimizer Experiment Matrix"
echo "===================================================="

# Array of datasets format: "filename:n_rows"
DATASETS=(
    "D1_vanilla:200000000"
    "D2_ext_a_target:200000000"
    "D3_ext_b_target:200000000"
    "D4_chaos:200000000"
    "scale_100M:100000000"
    "scale_300M:300000000"
    "scale_400M:400000000"
)

# Array of algorithm modes
MODES=("brute-force" "baseline" "ext-b" "ext-a" "ext-ab")

# Top-K to retrieve
K_VAL=50

for ds in "${DATASETS[@]}"; do
    # Parse filename and row count
    IFS=":" read -r NAME ROWS <<< "${ds}"
    FILE="data/${NAME}.bin"
    
    echo ""
    echo "----------------------------------------------------"
    echo " Evaluating Dataset: $NAME ($ROWS rows)"
    echo "----------------------------------------------------"

    if [ ! -f "$FILE" ]; then
        echo "⚠️ SKIPPING: $FILE not found. Did you run DATASETS.md?"
        continue
    fi

    for MODE in "${MODES[@]}"; do
        OUT_FILE="results/${NAME}_${MODE}.json"
        
        echo "[>] Running mode: $MODE"
        
        # Execute the C++ binary
        ./build/zippy \
            --input "$FILE" \
            --n-rows "$ROWS" \
            --k $K_VAL \
            --mode "$MODE" \
            --output "$OUT_FILE"
            
        # Note: 'ext-a' and 'ext-ab' will output a "Not Implemented" error
        # until Phase 5 and Phase 7 are complete, but the script will continue seamlessly.
    done
done

echo ""
echo "===================================================="
echo " Experiments Complete! Results saved to /results/"
echo " Run 'python python/plot_results.py' to generate graphs."
echo "===================================================="
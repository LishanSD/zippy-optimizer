# Phase 8: Running the Experiment Matrix

## Step 1: Prerequisites
Before running the experiments, ensure you have completed the following:
1. **Generated the Datasets:** You must have run the Python generation commands outlined in `DATASETS.md`. The binary files (e.g., `D1_vanilla.bin`) should exist in your `data/` directory.
2. **Compiled the Binary:** Ensure the latest version of the C++ engine is compiled with optimizations (`-O2` or `-O3`).
   ```bash
   g++ -std=c++17 -O2 -o build/zippy src/main.cpp src/zippy.cpp src/sampler.cpp src/measure_index.cpp -Isrc/
   
    ```

## Step 2: The Automation Script (`run_experiments.sh`)
To evaluate 5 algorithm modes across 7 different datasets, we need to run 35 distinct benchmarks. 

Save the following code as `run_experiments.sh` in the root of your `zippy-optimizer` folder. This script automates the entire matrix, matches row counts to the correct files, and names the output JSONs exactly how our plotting script expects them.

```bash
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
```

## Step 3: Execute the Experiments
Give the script execution permissions and run it from your terminal:

```bash
chmod +x run_experiments.sh
./run_experiments.sh
```

*Note: Depending on your CPU and memory speed, the brute-force and baseline modes on the 200M to 400M-row datasets may take upwards of 30-60 seconds each due to extreme L3 cache thrashing. Let the script run uninterrupted.*

## Step 4: Generate Plots & Tables
Once the bash script finishes, all your JSON files will be sitting in the `results/` folder. Use your Python scripts to parse them and generate your VLDB-style graphs and markdown tables:

```bash
python python/plot_results.py
python python/generate_tables.py
```
Check the `plots/` directory for your final PNG outputs and `results/PERFORMANCE_TABLES.md` for your detailed metric breakdown!
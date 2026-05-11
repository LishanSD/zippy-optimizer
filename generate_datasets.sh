#!/bin/bash

# Ensure the data directory exists
mkdir -p data

echo "===================================================="
echo " Generating VLDB-Scale Experiment Dataset Matrix"
echo " Note: This will generate several massive binary files"
echo " requiring roughly 25GB of disk space combined."
echo "===================================================="

echo "[1/7] Generating D1_vanilla (200M rows, 30M groups)..."
python python/generate_data.py --output data/D1_vanilla.bin --n-rows 200000000 --n-groups 30000000 --zipf-alpha 1.2 --rare-group-fraction 0.0

echo "[2/7] Generating D2_ext_a_target (200M rows, 30M groups)..."
python python/generate_data.py --output data/D2_ext_a_target.bin --n-rows 200000000 --n-groups 30000000 --zipf-alpha 1.1 --rare-group-fraction 0.05 --rare-group-rows 5 --rare-group-value-multiplier 500

echo "[3/7] Generating D3_ext_b_target (200M rows, 30M groups)..."
python python/generate_data.py --output data/D3_ext_b_target.bin --n-rows 200000000 --n-groups 30000000 --zipf-alpha 1.1 --rare-group-fraction 0.00001 --rare-group-rows 1 --rare-group-value-multiplier 1000000

echo "[4/7] Generating D4_chaos (200M rows, 30M groups)..."
python python/generate_data.py --output data/D4_chaos.bin --n-rows 200000000 --n-groups 30000000 --zipf-alpha 1.05 --rare-group-fraction 0.02 --rare-group-rows 3 --rare-group-value-multiplier 50000

echo "[5/7] Generating scale_100M (100M rows, 15M groups)..."
python python/generate_data.py --output data/scale_100M.bin --n-rows 100000000 --n-groups 15000000 --zipf-alpha 1.05 --rare-group-fraction 0.02 --rare-group-rows 3 --rare-group-value-multiplier 50000

echo "[6/7] Generating scale_300M (300M rows, 37M groups)..."
python python/generate_data.py --output data/scale_300M.bin --n-rows 300000000 --n-groups 37000000 --zipf-alpha 1.05 --rare-group-fraction 0.02 --rare-group-rows 3 --rare-group-value-multiplier 50000

echo "[7/7] Generating scale_400M (400M rows, 55M groups)..."
python python/generate_data.py --output data/scale_400M.bin --n-rows 400000000 --n-groups 55000000 --zipf-alpha 1.05 --rare-group-fraction 0.02 --rare-group-rows 3 --rare-group-value-multiplier 50000

echo "===================================================="
echo " All datasets generated successfully in the /data directory!"
echo "===================================================="
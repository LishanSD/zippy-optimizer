import os
import subprocess
import json
import sys

def get_values(results_file):
    with open(results_file, 'r') as f:
        data = json.load(f)
        vals = [r['aggregate'] for r in data['top_k_results']]
        return sorted(vals, reverse=True)

def run_zippy(dataset, n_rows, mode, k=10, out_file='out.json'):
    exe = './build/zippy.exe' if os.name == 'nt' else './build/zippy'
    cmd = [
        exe,
        '--input', dataset,
        '--n-rows', str(n_rows),
        '--k', str(k),
        '--mode', mode,
        '--output', out_file
    ]
    # run with ./build/zippy
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return get_values(out_file)

def verify():
    datasets = {
        'S0': ('data/S0.bin', 10089),
        'S1': ('data/S1.bin', 1000000),
        'S2': ('data/S2.bin', 2000000)
    }
    
    modes = ['baseline', 'ext-a', 'ext-b', 'ext-ab']
    all_passed = True
    
    print("Starting Correctness Verification Sweep...")
    
    for ds_name, (ds_path, ds_rows) in datasets.items():
        if not os.path.exists(ds_path):
            print(f"Skipping {ds_name} — file not found")
            continue
            
        print(f"\nVerifying {ds_name}...")
        bf_file = f'results/{ds_name}_bf.json'
        
        try:
            bf_vals = run_zippy(ds_path, ds_rows, 'brute-force', k=50, out_file=bf_file)
        except Exception as e:
            print(f"  [ERROR] Brute-force failed on {ds_name}: {e}")
            all_passed = False
            continue
            
        for mode in modes:
            out_file = f'results/{ds_name}_{mode}.json'
            try:
                mode_vals = run_zippy(ds_path, ds_rows, mode, k=50, out_file=out_file)
                if mode_vals == bf_vals:
                    print(f"  [PASS] {mode} matches brute-force")
                else:
                    print(f"  [FAIL] {mode} mismatch!")
                    all_passed = False
            except Exception as e:
                print(f"  [ERROR] {mode} failed: {e}")
                all_passed = False
                
    if all_passed:
        print("\nAll correctness tests passed!")
        sys.exit(0)
    else:
        print("\nSome correctness tests failed.")
        sys.exit(1)

if __name__ == '__main__':
    verify()

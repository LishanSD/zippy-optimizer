import json

with open('results/S2_bf.json') as f:       bf  = json.load(f)
with open('results/S2_baseline.json') as f:  bl  = json.load(f)
with open('results/S2_ext_a.json') as f:     ea  = json.load(f)

bm  = bf['metrics']
blm = bl['metrics']
eam = ea['metrics']

bf_ids  = set(r['group_id'] for r in bf['top_k_results'])
bl_ids  = set(r['group_id'] for r in bl['top_k_results'])
ea_ids  = set(r['group_id'] for r in ea['top_k_results'])

print("=== S2 comparison (k=20) ===")
print(f"  Brute-force top-20:  {sorted(bf_ids)}")
print(f"  Baseline   top-20:  {sorted(bl_ids)}")
print(f"  ext-a      top-20:  {sorted(ea_ids)}")
print()
print(f"  Baseline matches BF:  {bl_ids == bf_ids}")
print(f"  ext-a    matches BF:  {ea_ids == bf_ids}")
print()
print(f"{'Metric':<35} {'BF':>12} {'Baseline':>12} {'ext-a':>12}")
print("-" * 75)

def m(d, key, fmt=".1f"):
    v = d.get(key, "—")
    if v == "—": return v
    return format(v, fmt)

rows = [
    ("total_duration_ms",        "ms"),
    ("topKBound_after_pass1",    ""),
    ("partitions_pruned_pct",    "%"),
    ("total_passes",             ""),
    ("index_build_duration_ms",  "ms"),
]

for key, unit in rows:
    bfv  = bm.get(key,  "—")
    blv  = blm.get(key, "—")
    eav  = eam.get(key, "—")
    if key == "partitions_pruned_pct":
        bfv  = f"{bfv*100:.1f}%"  if isinstance(bfv, float)  else "—"
        blv  = f"{blv*100:.1f}%"  if isinstance(blv, float)  else "—"
        eav  = f"{eav*100:.1f}%"  if isinstance(eav, float)  else "—"
    elif isinstance(bfv,  float): bfv  = f"{bfv:.1f}"
    elif isinstance(blv,  float): blv  = f"{blv:.1f}"
    elif isinstance(eav,  float): eav  = f"{eav:.1f}"
    print(f"  {key:<33} {str(bfv):>12} {str(blv):>12} {str(eav):>12}")

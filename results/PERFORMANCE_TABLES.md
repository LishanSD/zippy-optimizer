# Zippy Optimizer Experiment Results

## 1. Execution Summary
This table highlights the convergence efficiency and overall execution time.

| Dataset         |        Rows | Mode        |   Passes |   Pruned (%) |   Top-K Bound |   Total Time (ms) |
|:----------------|------------:|:------------|---------:|-------------:|--------------:|------------------:|
| D1_vanilla      | 200,000,000 | brute-force |        0 |         0    |             0 |            7868.3 |
| D1_vanilla      | 200,000,000 | baseline    |        1 |       100    |    32,787,851 |            4367   |
| D1_vanilla      | 200,000,000 | ext-a       |        1 |       100    |    32,787,851 |            6143   |
| D1_vanilla      | 200,000,000 | ext-b       |        1 |       100    |    32,787,851 |            4935.2 |
| D1_vanilla      | 200,000,000 | ext-ab      |        1 |       100    |    32,787,851 |            4446.9 |
| D2_ext_a_target | 204,497,519 | brute-force |        0 |         0    |             0 |           19811.7 |
| D2_ext_a_target | 204,497,519 | baseline    |        2 |        99.99 |    26,244,794 |           11973.9 |
| D2_ext_a_target | 204,497,519 | ext-a       |        2 |        99.98 |    26,267,854 |           16319.9 |
| D2_ext_a_target | 204,497,519 | ext-b       |        2 |        99.99 |    26,244,794 |           14910.1 |
| D2_ext_a_target | 204,497,519 | ext-ab      |        2 |        99.99 |    26,244,794 |           14873.9 |
| D3_ext_b_target | 200,000,300 | brute-force |        0 |         0    |             0 |           19426.8 |
| D3_ext_b_target | 200,000,300 | baseline    |        2 |        99.64 |   100,698,249 |           11618.9 |
| D3_ext_b_target | 200,000,300 | ext-a       |        2 |        99.64 |   100,853,618 |           15697.2 |
| D3_ext_b_target | 200,000,300 | ext-b       |        1 |       100    |   100,000,000 |            6501   |
| D3_ext_b_target | 200,000,300 | ext-ab      |        1 |       100    |   100,000,000 |            5862.1 |
| D4_chaos        | 201,199,864 | brute-force |        0 |         0    |             0 |           23764.9 |
| D4_chaos        | 201,199,864 | baseline    |        0 |         0    |             0 |           24421.3 |
| D4_chaos        | 201,199,864 | ext-a       |        0 |         0    |             0 |           28742.8 |
| D4_chaos        | 201,199,864 | ext-b       |        0 |         0    |             0 |           24240.7 |
| D4_chaos        | 201,199,864 | ext-ab      |        0 |         0    |             0 |           25295.2 |
| scale_100M      | 100,600,002 | brute-force |        0 |         0    |             0 |           10578.9 |
| scale_100M      | 100,600,002 | baseline    |        2 |        99.51 |   366,321,256 |            7591.8 |
| scale_100M      | 100,600,002 | ext-a       |        2 |        99.51 |   371,325,188 |           10968.4 |
| scale_100M      | 100,600,002 | ext-b       |        2 |        99.51 |   366,321,256 |            9231.4 |
| scale_100M      | 100,600,002 | ext-ab      |        2 |        99.51 |   366,321,256 |            9580.3 |
| scale_300M      | 301,479,749 | brute-force |        0 |         0    |             0 |           35086.3 |
| scale_300M      | 301,479,749 | baseline    |        0 |         0    |             0 |           35958   |
| scale_300M      | 301,479,749 | ext-a       |        0 |         0    |             0 |           42081.7 |
| scale_300M      | 301,479,749 | ext-b       |        0 |         0    |             0 |           36274   |
| scale_300M      | 301,479,749 | ext-ab      |        0 |         0    |             0 |           38358.6 |
| scale_400M      | 402,199,090 | brute-force |        0 |         0    |             0 |           56134.7 |
| scale_400M      | 402,199,090 | baseline    |        0 |         0    |             0 |           58482.9 |
| scale_400M      | 402,199,090 | ext-a       |        0 |         0    |             0 |          572364   |
| scale_400M      | 402,199,090 | ext-b       |        0 |         0    |             0 |           59751.7 |
| scale_400M      | 402,199,090 | ext-ab      |        0 |         0    |             0 |          398807   |

---

## 2. Timing Breakdown
This table breaks down the computational phases to demonstrate the overhead versus savings of the extensions.

| Dataset         | Mode        |   Index Build (ms) |   Sample Time (ms) |   Pass 1 (ms) |   Pass 2+ (ms) |   Total Time (ms) |
|:----------------|:------------|-------------------:|-------------------:|--------------:|---------------:|------------------:|
| D1_vanilla      | brute-force |                0   |                0   |           0   |            0   |            7868.3 |
| D1_vanilla      | baseline    |                0   |              280.3 |        4063   |            0   |            4367   |
| D1_vanilla      | ext-a       |             9236.3 |              913.3 |        5203.8 |            0   |            6143   |
| D1_vanilla      | ext-b       |              508.4 |              230.4 |        4174.8 |            0   |            4935.2 |
| D1_vanilla      | ext-ab      |            10180.4 |              227   |        4193.7 |            0   |            4446.9 |
| D2_ext_a_target | brute-force |                0   |                0   |           0   |            0   |           19811.7 |
| D2_ext_a_target | baseline    |                0   |              408.9 |        6065.2 |         5453   |           11973.9 |
| D2_ext_a_target | ext-a       |            26509.3 |             2516.5 |        6096.6 |         7652.2 |           16319.9 |
| D2_ext_a_target | ext-b       |              466.6 |              406.5 |        6346.5 |         7648.6 |           14910.1 |
| D2_ext_a_target | ext-ab      |            29483.5 |              640.6 |        6382   |         7800.3 |           14873.9 |
| D3_ext_b_target | brute-force |                0   |                0   |           0   |            0   |           19426.8 |
| D3_ext_b_target | baseline    |                0   |              396.3 |        5480.9 |         5671   |           11618.9 |
| D3_ext_b_target | ext-a       |            25980.7 |             2781.5 |        5313.5 |         7522.1 |           15697.2 |
| D3_ext_b_target | ext-b       |              580.9 |              403.7 |        5476.2 |            0   |            6501   |
| D3_ext_b_target | ext-ab      |            27871.5 |              545.7 |        5269.1 |            0   |            5862.1 |
| D4_chaos        | brute-force |                0   |                0   |           0   |            0   |           23764.9 |
| D4_chaos        | baseline    |                0   |              481.8 |           0   |            0   |           24421.3 |
| D4_chaos        | ext-a       |            40734.4 |             3356.2 |           0   |            0   |           28742.8 |
| D4_chaos        | ext-b       |              459.1 |              440   |           0   |            0   |           24240.7 |
| D4_chaos        | ext-ab      |            45990.9 |              566.8 |           0   |            0   |           25295.2 |
| scale_100M      | brute-force |                0   |                0   |           0   |            0   |           10578.9 |
| scale_100M      | baseline    |                0   |              272.5 |        3265.7 |         3985.9 |            7591.8 |
| scale_100M      | ext-a       |            19371.1 |             1670.4 |        3417.9 |         5786.9 |           10968.4 |
| scale_100M      | ext-b       |              223.9 |              295.9 |        3257.7 |         5384.9 |            9231.4 |
| scale_100M      | ext-ab      |            22193.4 |              329.3 |        3469.8 |         5709.8 |            9580.3 |
| scale_300M      | brute-force |                0   |                0   |           0   |            0   |           35086.3 |
| scale_300M      | baseline    |                0   |              897.2 |           0   |            0   |           35958   |
| scale_300M      | ext-a       |            62949   |             4612.1 |           0   |            0   |           42081.7 |
| scale_300M      | ext-b       |              699.8 |              682.2 |           0   |            0   |           36274   |
| scale_300M      | ext-ab      |            72205.3 |             1258.6 |           0   |            0   |           38358.6 |
| scale_400M      | brute-force |                0   |                0   |           0   |            0   |           56134.7 |
| scale_400M      | baseline    |                0   |             1304   |           0   |            0   |           58482.9 |
| scale_400M      | ext-a       |           149190   |           468267   |           0   |            0   |          572364   |
| scale_400M      | ext-b       |             1277.1 |             1002.6 |           0   |            0   |           59751.7 |
| scale_400M      | ext-ab      |           158424   |           326164   |           0   |            0   |          398807   |

$ErrorActionPreference = "Stop"

$resultsDir = "results"
if (-not (Test-Path $resultsDir)) {
    New-Item -ItemType Directory -Path $resultsDir | Out-Null
}

$zippyBin = Join-Path "build" "zippy.exe"
if (-not (Test-Path $zippyBin)) {
    throw "Missing binary: $zippyBin. Build the project first."
}

Write-Host "===================================================="
Write-Host " Starting VLDB-Scale Zippy Optimizer Experiment Matrix"
Write-Host "===================================================="
Write-Host " Using binary: $zippyBin"

$datasets = @(
    "D1_vanilla",
    "D2_ext_a_target",
    "D3_ext_b_target",
    "D4_chaos",
    "scale_100M",
    "scale_300M",
    "scale_400M"
)

$modes = @("brute-force", "baseline", "ext-b", "ext-a", "ext-ab")
$kVal = 50
$rowSizeBytes = 16

foreach ($name in $datasets) {
    $file = Join-Path "data" "$name.bin"

    if (-not (Test-Path $file)) {
        Write-Host "SKIPPING: $file not found. Did you run DATASETS.md?"
        continue
    }

    $fileSizeBytes = (Get-Item $file).Length
    if (($fileSizeBytes % $rowSizeBytes) -ne 0) {
        Write-Host "SKIPPING: $file size ($fileSizeBytes bytes) is not divisible by $rowSizeBytes."
        Write-Host "This does not look like a valid Zippy binary dataset."
        continue
    }

    $rows = [Int64]($fileSizeBytes / $rowSizeBytes)

    Write-Host ""
    Write-Host "----------------------------------------------------"
    Write-Host " Evaluating Dataset: $name ($rows rows)"
    Write-Host "----------------------------------------------------"

    foreach ($mode in $modes) {
        $outFile = Join-Path $resultsDir "${name}_${mode}.json"

        Write-Host "[>] Running mode: $mode"

        & $zippyBin `
            --input $file `
            --n-rows $rows `
            --k $kVal `
            --mode $mode `
            --output $outFile

        if ($LASTEXITCODE -ne 0) {
            throw "zippy.exe failed for dataset '$name' in mode '$mode'."
        }
    }
}

Write-Host ""
Write-Host "===================================================="
Write-Host " Experiments Complete! Results saved to /results/"
Write-Host " Run 'python python/plot_results.py' to generate graphs."
Write-Host "===================================================="

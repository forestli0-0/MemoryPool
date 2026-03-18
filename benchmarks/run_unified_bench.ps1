param(
    [string]$BuildDir = "benchmarks/build-ucrt64"
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$cmake = 'C:\msys64\ucrt64\bin\cmake.exe'
$ninja = 'C:\msys64\ucrt64\bin\ninja.exe'
$gcc = 'C:\msys64\ucrt64\bin\gcc.exe'
$gxx = 'C:\msys64\ucrt64\bin\g++.exe'
$env:PATH = 'C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH

$buildPath = Join-Path $repoRoot $BuildDir
if (!(Test-Path $buildPath)) {
    & $cmake -S (Join-Path $repoRoot 'benchmarks') -B $buildPath -G Ninja `
        -DCMAKE_MAKE_PROGRAM=$ninja `
        -DCMAKE_C_COMPILER=$gcc `
        -DCMAKE_CXX_COMPILER=$gxx
}

& $cmake --build $buildPath --target unified_v1_bench unified_v3_bench unified_v4_bench

$targets = @('unified_v1_bench.exe', 'unified_v3_bench.exe', 'unified_v4_bench.exe')
$rows = @()
foreach ($target in $targets) {
    $exe = Join-Path $buildPath $target
    Write-Host "=== $target ==="
    $output = & $exe
    $output | ForEach-Object { Write-Host $_ }

    foreach ($line in $output) {
        if ($line -match '^RESULT\|([^|]+)\|([^|]+)\|([0-9.]+)\|([0-9.]+)\|([0-9.]+)$') {
            $rows += [pscustomobject]@{
                Version = $matches[1]
                Scenario = $matches[2]
                AllocatorMs = [double]$matches[3]
                SystemMs = [double]$matches[4]
                Ratio = [double]$matches[5]
            }
        }
    }
    Write-Host ''
}

Write-Host '=== Markdown Table ==='
Write-Host '| Version | Scenario | Allocator ms | new/delete ms | Ratio |'
Write-Host '|---|---|---:|---:|---:|'
foreach ($row in $rows | Sort-Object Version, Scenario) {
    Write-Host ("| {0} | {1} | {2:N3} | {3:N3} | {4:N3}x |" -f $row.Version, $row.Scenario, $row.AllocatorMs, $row.SystemMs, $row.Ratio)
}

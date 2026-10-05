# Builds Release and creates dist\VirtualPrism-<version>-win64.zip
# Run: powershell -ExecutionPolicy Bypass -File scripts\package.ps1
param([string]$Version = "1.0.0")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio" -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $cmake) { throw "cmake not found. Install CMake or the Visual Studio C++ workload." }

& $cmake -S $root -B "$root\build" -A x64 | Out-Null
& $cmake --build "$root\build" --config Release --target VirtualPrism
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$stage = "$root\dist\VirtualPrism-$Version-win64"
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory $stage -Force | Out-Null
foreach ($f in "VirtualPrism.exe", "XR_APILAYER_NOVENDOR_virtual_prism.dll", "XR_APILAYER_NOVENDOR_virtual_prism.json", "openxr_loader.dll") {
    Copy-Item "$root\build\Release\$f" $stage
}
Copy-Item "$root\LICENSE", "$root\README.md" $stage
Compress-Archive -Path "$stage\*" -DestinationPath "$stage.zip" -Force
Write-Host "Created $stage.zip"

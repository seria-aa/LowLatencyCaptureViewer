param(
    [string]$BuildDir = "..\build-v100-release",
    [string]$OutputDir = "..\outputs\v1.0.1"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot ".." )).Path
$build = (Resolve-Path (Join-Path $PSScriptRoot $BuildDir)).Path
$output = Join-Path $root $OutputDir
$portable = Join-Path $output "LowLatencyCaptureViewer_v1.0.1_x64"
$zip = Join-Path $output "LowLatencyCaptureViewer_v1.0.1_x64.zip"
$exe = Join-Path $build "LowLatencyCaptureViewer.exe"

if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Release executable not found: $exe"
}

New-Item -ItemType Directory -Force -Path $output | Out-Null
if (Test-Path -LiteralPath $portable) {
    $resolvedPortable = (Resolve-Path -LiteralPath $portable).Path
    $resolvedOutput = (Resolve-Path -LiteralPath $output).Path
    if (-not $resolvedPortable.StartsWith($resolvedOutput + [IO.Path]::DirectorySeparatorChar,
                                           [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a path outside the generated output directory."
    }
    Remove-Item -LiteralPath $resolvedPortable -Recurse -Force
}
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
New-Item -ItemType Directory -Force -Path $portable | Out-Null

$files = @("README.md", "README.ko.md", "LICENSE", "실행안내.txt", "DEPENDENCIES.txt")
Copy-Item -LiteralPath $exe -Destination $portable
foreach ($file in $files) {
    Copy-Item -LiteralPath (Join-Path $root $file) -Destination $portable
}
Copy-Item -LiteralPath (Join-Path $root "docs") -Destination $portable -Recurse
Compress-Archive -Path (Join-Path $portable "*") -DestinationPath $zip -CompressionLevel Optimal
Write-Host "Portable package: $zip"

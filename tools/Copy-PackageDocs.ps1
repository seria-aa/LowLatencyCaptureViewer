param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$DestinationRoot
)

$ErrorActionPreference = "Stop"
$source = (Resolve-Path -LiteralPath $SourceRoot).Path
$destination = [IO.Path]::GetFullPath($DestinationRoot)
if ($source.TrimEnd('\', '/') -eq $destination.TrimEnd('\', '/')) {
    throw "Documentation staging must not overwrite the source tree."
}
foreach ($directory in @("docs", "tests")) {
    $inputDirectory = Join-Path $source $directory
    if ($destination -eq $inputDirectory -or
        $destination.StartsWith($inputDirectory + [IO.Path]::DirectorySeparatorChar,
                                [StringComparison]::OrdinalIgnoreCase)) {
        throw "Documentation staging must not be inside $directory."
    }
}

# Keep this root list in sync with the installer and CMake install rules.
$rootFiles = @("README.md", "README.ko.md", "README.en.md", "LICENSE", "실행안내.txt", "DEPENDENCIES.txt")
foreach ($relative in $rootFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $relative) -PathType Leaf)) {
        throw "Missing required package input: $relative"
    }
}
$docs = Join-Path $source "docs"
if (-not (Test-Path -LiteralPath $docs -PathType Container)) {
    throw "Missing required package input: docs"
}
# Development records remain available through the index's repository links.
# Keep exclusions in sync with the installer and CMake install rules.
$excludedDocs = @("rounded-corners-validation.md", "TEST-BLT.ko.md",
    "vsr-all-formats-validation.md", "vsr-experiment.md", "vsr-hdr-validation.md",
    "SPATIAL_UPSCALING_EXPERIMENT.md", "SPATIAL_UPSCALING_EXTENDED_COMPARISON.md",
    "SPATIAL_UPSCALING_FHD_RECOMPARISON.md",
    "UPSCALING_BOUNDARY_SAFETY.md", "UPSCALING_OVERFLOW_DIAGNOSTIC.md", "UYVY_HDYC_SUPPORT.md",
    "RGB_CAPTURE_SUPPORT.md",
    "images/viewer.png")

New-Item -ItemType Directory -Force -Path $destination | Out-Null
foreach ($relative in $rootFiles) {
    Copy-Item -LiteralPath (Join-Path $source $relative) -Destination $destination
}
foreach ($notice in @(
    @("third_party/upscaling/nis/licence.txt", "NIS-LICENSE.txt"),
    @("third_party/upscaling/cmaa2/license.txt", "CMAA2-LICENSE.txt"),
    @("third_party/upscaling/NOTICE.txt", "UPSCALING-NOTICE.txt"))) {
    Copy-Item -LiteralPath (Join-Path $source $notice[0]) -Destination (Join-Path $destination $notice[1])
}
foreach ($document in Get-ChildItem -LiteralPath $docs -Recurse -File) {
    $relative = $document.FullName.Substring($docs.Length + 1).Replace('\', '/')
    if ($relative -in $excludedDocs) { continue }
    $target = Join-Path $destination ("docs/" + $relative)
    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
    Copy-Item -LiteralPath $document.FullName -Destination $target
}

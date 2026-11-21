param(
    [string]$Version = "2.4.1",
    [string]$BuildDir = "..\build-v2410-release",
    [string]$OutputDir = "outputs\v2.4.1"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot ".." )).Path
$build = (Resolve-Path (Join-Path $PSScriptRoot $BuildDir)).Path
$output = Join-Path $root $OutputDir
$portable = Join-Path $output "LowLatencyCaptureViewer_v$Version`_x64"
$zip = Join-Path $output "LowLatencyCaptureViewer_v$Version`_x64.zip"
$exe = Join-Path $build "LowLatencyCaptureViewer.exe"

if (Test-Path -LiteralPath (Join-Path $build 'llcv-audio-recovery-diagnostic.txt')) {
    throw "Audio-input recovery diagnostic builds cannot be packaged as a regular release."
}
if ((Get-ChildItem -LiteralPath $build -Filter 'swresample-*.dll' -File) -or
    (Test-Path -LiteralPath (Join-Path $build 'ffmpeg-licenses/dolby-ffmpeg-NOTICE.txt'))) {
    throw "Dolby preview FFmpeg cannot be packaged as a regular release."
}
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Release executable not found: $exe"
}
$exeVersion = (Get-Item -LiteralPath $exe).VersionInfo.FileVersion
if ($exeVersion -ne $Version -and $exeVersion -ne "$Version.0") {
    throw "Version mismatch: requested $Version but executable reports $exeVersion"
}
if ([version]$Version -ge [version]'2.3.0') {
    $stamp = Join-Path $build 'llcv-release-vfg-sm86.sha256'
    if (-not (Test-Path -LiteralPath $stamp -PathType Leaf)) {
        throw 'Build the VFG + SM86 release executable before packaging.'
    }
    if ((Get-Content -LiteralPath $stamp -Raw).Trim() -ne (Get-FileHash -LiteralPath $exe).Hash) {
        throw 'VFG release stamp does not match the executable; rebuild before packaging.'
    }
}

$rifeBundle = Join-Path $build 'rife-vulkan'
$rifeAssets = @('fast-fold-cache.ncnn.param','fast-fold-cache.ncnn.bin',
    'fast-encoder.ncnn.param','fast-encoder.ncnn.bin','RIFE-MODEL-LICENSE.txt','NCNN-LICENSE.txt','GLSLANG-LICENSE.txt')
$includeRife = [version]$Version -ge [version]'2.4.0' -or
    (Test-Path -LiteralPath (Join-Path $rifeBundle 'fast-fold-cache.ncnn.bin') -PathType Leaf)
if ($includeRife) {
    $rifeStamp = Join-Path $build 'llcv-release-rife-vulkan.sha256'
    if (!(Test-Path -LiteralPath $rifeStamp -PathType Leaf) -or
        (Get-Content -LiteralPath $rifeStamp -TotalCount 1).Trim() -ne (Get-FileHash -LiteralPath $exe).Hash) {
        throw 'Build the bundled RIFE Vulkan executable before packaging.'
    }
    foreach ($asset in $rifeAssets) {
        if (!(Test-Path -LiteralPath (Join-Path $rifeBundle $asset) -PathType Leaf)) {
            throw "Missing bundled RIFE file: $asset"
        }
    }
}

New-Item -ItemType Directory -Force -Path $output | Out-Null
foreach ($required in @("avformat-62.dll", "avcodec-62.dll", "avutil-60.dll",
        "ffmpeg-licenses/COPYING.LGPLv2.1", "ffmpeg-licenses/LICENSE.md",
        "ffmpeg-licenses/recording-ffmpeg-NOTICE.txt", "ffmpeg-licenses/config.mak")) {
    if (-not (Test-Path -LiteralPath (Join-Path $build $required) -PathType Leaf)) {
        throw "Incomplete recording release build: $required"
    }
}
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

Copy-Item -LiteralPath $exe -Destination $portable
if ($includeRife) {
    $rifeTarget = Join-Path $portable 'rife-vulkan'
    New-Item -ItemType Directory -Path $rifeTarget | Out-Null
    foreach ($asset in $rifeAssets) {
        Copy-Item -LiteralPath (Join-Path $rifeBundle $asset) -Destination $rifeTarget
    }
}
# Only the pinned mux runtime is shipped, never the development SDK or FFmpeg tools.
foreach ($dll in @("avformat-62.dll", "avcodec-62.dll", "avutil-60.dll")) {
    Copy-Item -LiteralPath (Join-Path $build $dll) -Destination $portable
}
Copy-Item -LiteralPath (Join-Path $build "ffmpeg-licenses") -Destination $portable -Recurse
& (Join-Path $PSScriptRoot "Copy-PackageDocs.ps1") -SourceRoot $root -DestinationRoot $portable
Copy-Item -LiteralPath (Join-Path $root "third_party\asio\LICENSE.txt") `
    -Destination (Join-Path $portable "ASIO-SDK-LICENSE.txt")
Copy-Item -LiteralPath (Join-Path $root "third_party\asio\HOST-LICENSE.txt") `
    -Destination (Join-Path $portable "ASIO-HOST-LICENSE.txt")
Copy-Item -LiteralPath (Join-Path $root "third_party\pretendard\LICENSE.txt") `
    -Destination (Join-Path $portable "PRETENDARD-LICENSE.txt")
& (Join-Path $PSScriptRoot "Test-PackageDocs.ps1") -PackageRoot $portable | Out-Null
& (Join-Path $PSScriptRoot "Test-ReleasePackage.ps1") -PackageRoot $portable -Version $Version | Out-Null
Compress-Archive -Path (Join-Path $portable "*") -DestinationPath $zip -CompressionLevel Optimal
Write-Host "Portable package: $zip"

param(
    [Parameter(Mandatory = $true)][string]$PackageRoot,
    [string]$Version = "2.4.1"
)
$ErrorActionPreference = "Stop"
$package = (Resolve-Path -LiteralPath $PackageRoot).Path
$required = @("LowLatencyCaptureViewer.exe", "avformat-62.dll", "avcodec-62.dll", "avutil-60.dll",
    "LICENSE", "ASIO-SDK-LICENSE.txt", "ASIO-HOST-LICENSE.txt", "PRETENDARD-LICENSE.txt",
    "NIS-LICENSE.txt", "CMAA2-LICENSE.txt", "UPSCALING-NOTICE.txt",
    "ffmpeg-licenses/COPYING.LGPLv2.1", "ffmpeg-licenses/LICENSE.md",
    "ffmpeg-licenses/recording-ffmpeg-NOTICE.txt", "ffmpeg-licenses/config.mak",
    "README.md", "README.ko.md", "README.en.md", "docs/RECORDING.md", "docs/RECORDING.ko.md")
if ([version]$Version -ge [version]'2.3.0') { $required += 'VFG-RTX30-NOTICE.txt' }
$rifeHashes = @{
    'rife-vulkan/fast-fold-cache.ncnn.param' = '7555b43762cd23cb38139f0719fe72aa1d599c862c86c2f255ce451999523b9c'
    'rife-vulkan/fast-fold-cache.ncnn.bin' = '95d45db607cb8e0896f10990e5b6667a88ca64429ea4b00349f41211f7775a93'
    'rife-vulkan/fast-encoder.ncnn.param' = 'ee51d9303fb015bfb888b634436e75b9ec0e9bc3fcb48d5eb7b1c0f0b4b83c01'
    'rife-vulkan/fast-encoder.ncnn.bin' = '16af9ff58f802d0b1050ed2289ee537fc359cbf3d52e29b3938273fac6a36a23'
}
if ([version]$Version -ge [version]'2.4.0' -or
    (Test-Path -LiteralPath (Join-Path $package 'rife-vulkan') -PathType Container)) {
    $required += @($rifeHashes.Keys) + @('rife-vulkan/RIFE-MODEL-LICENSE.txt',
        'rife-vulkan/NCNN-LICENSE.txt','rife-vulkan/GLSLANG-LICENSE.txt')
}
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $relative) -PathType Leaf)) {
        throw "Missing release file: $relative"
    }
    if ($rifeHashes.ContainsKey($relative) -and
        (Get-FileHash -LiteralPath (Join-Path $package $relative)).Hash -ne $rifeHashes[$relative]) {
        throw "Unexpected RIFE model hash: $relative"
    }
}
$actualVersion = (Get-Item -LiteralPath (Join-Path $package "LowLatencyCaptureViewer.exe")).VersionInfo.FileVersion
if ($actualVersion -ne $Version -and $actualVersion -ne "$Version.0") {
    throw "Wrong executable version: $actualVersion"
}
$runtime = @("LowLatencyCaptureViewer.exe", "avformat-62.dll", "avcodec-62.dll", "avutil-60.dll")
foreach ($file in Get-ChildItem -LiteralPath $package -Recurse -File) {
    $relative = $file.FullName.Substring($package.Length + 1).Replace('\', '/')
    if (($file.Extension -in @(".exe", ".dll") -and $relative -notin $runtime) -or
        $file.Extension -in @(".pdb", ".ilk", ".obj", ".log", ".ini", ".lib", ".exp", ".onnx", ".engine", ".plan", ".pt") -or
        $relative -match '(^|/)(vfg|nvidia-rife)(/|$)' -or
        ($relative -like 'rife-vulkan/*' -and $relative -notin $required)) {
        throw "Unexpected release file: $relative"
    }
    if ([version]$Version -ge [version]'2.3.0' -and ($relative -in $runtime -or $relative -eq 'ffmpeg-licenses/config.mak')) {
        # ASCII and UTF-16LE (at either byte alignment) can occur in PE metadata.
        $bytes = [IO.File]::ReadAllBytes($file.FullName)
        $profilePath = '(?i)(?:[a-z]:[\\/]|/[a-z]/)Users[\\/]'
        if ([Text.Encoding]::ASCII.GetString($bytes) -match $profilePath -or
            [Text.Encoding]::Unicode.GetString($bytes) -match $profilePath -or
            ($bytes.Length -gt 1 -and [Text.Encoding]::Unicode.GetString($bytes,1,$bytes.Length-1) -match $profilePath)) {
            throw "Build-machine profile path in release file: $relative"
        }
    }
}
& (Join-Path $PSScriptRoot "Test-PackageDocs.ps1") -PackageRoot $package | Out-Null
[pscustomobject]@{ Version = $actualVersion; RequiredFiles = $required.Count; Root = $package }

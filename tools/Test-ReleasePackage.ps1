param(
    [Parameter(Mandatory = $true)][string]$PackageRoot,
    [string]$Version = "2.1.5"
)
$ErrorActionPreference = "Stop"
$package = (Resolve-Path -LiteralPath $PackageRoot).Path
$required = @("LowLatencyCaptureViewer.exe", "avformat-62.dll", "avcodec-62.dll", "avutil-60.dll",
    "LICENSE", "ASIO-SDK-LICENSE.txt", "ASIO-HOST-LICENSE.txt", "PRETENDARD-LICENSE.txt",
    "ffmpeg-licenses/COPYING.LGPLv2.1", "ffmpeg-licenses/LICENSE.md",
    "ffmpeg-licenses/recording-ffmpeg-NOTICE.txt", "ffmpeg-licenses/config.mak",
    "README.md", "README.ko.md", "docs/RECORDING.md", "docs/RECORDING.ko.md")
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $package $relative) -PathType Leaf)) {
        throw "Missing release file: $relative"
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
        $file.Extension -in @(".pdb", ".ilk", ".obj", ".log", ".ini", ".lib", ".exp")) {
        throw "Unexpected release file: $relative"
    }
}
& (Join-Path $PSScriptRoot "Test-PackageDocs.ps1") -PackageRoot $package | Out-Null
[pscustomobject]@{ Version = $actualVersion; RequiredFiles = $required.Count; Root = $package }

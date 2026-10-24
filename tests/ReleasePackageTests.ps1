param(
    [Parameter(Mandatory = $true)][string]$BuildRoot,
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Configuration = "Release",
    [string]$CMake = "cmake"
)
$ErrorActionPreference = "Stop"
$build = (Resolve-Path -LiteralPath $BuildRoot).Path
$stage = Join-Path $build ("package-test-" + [Guid]::NewGuid().ToString("N"))
$validator = Join-Path $SourceRoot "tools/Test-ReleasePackage.ps1"
try {
    & $CMake --install $build --config $Configuration --prefix $stage
    if ($LASTEXITCODE -ne 0) { throw "Release staging failed." }
    & $validator -PackageRoot $stage -Version $Version
    # A missing mux DLL must fail validation, not produce a playback-only release.
    $runtime = Join-Path $stage "avutil-60.dll"
    $backup = Join-Path $stage "avutil-test-backup.bin"
    Move-Item -LiteralPath $runtime -Destination $backup
    try {
        $rejected = $false
        try { & $validator -PackageRoot $stage -Version $Version | Out-Null }
        catch { $rejected = $_.Exception.Message.Contains("avutil-60.dll") }
        if (-not $rejected) { throw "A missing recording runtime was not detected." }
    } finally { Move-Item -LiteralPath $backup -Destination $runtime }
    & $validator -PackageRoot $stage -Version $Version

    # A diagnostic build directory may retain an older regular executable.
    # The marker must reject it before version checks or any output creation.
    # Keep both the fake build and its possible outputs inside the validated
    # staging directory so the existing finally block owns all fixture cleanup.
    $diagnosticBuild = Join-Path $stage 'diagnostic-build'
    $diagnosticOutput = Join-Path $stage 'diagnostic-output'
    New-Item -ItemType Directory -Path $diagnosticBuild | Out-Null
    Copy-Item -LiteralPath (Join-Path $stage 'LowLatencyCaptureViewer.exe') `
        -Destination $diagnosticBuild
    [IO.File]::WriteAllText((Join-Path $diagnosticBuild 'llcv-audio-recovery-diagnostic.txt'),
        'Diagnostic-only build fixture.')

    # package-v1 resolves its build argument from tools/ and its output argument
    # from the repository root. Use relative paths for both (including spaces).
    $source = (Resolve-Path -LiteralPath $SourceRoot).Path
    $toolsDirectory = Join-Path $source 'tools'
    $toolsUri = [Uri]($toolsDirectory.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar)
    $sourceUri = [Uri]($source.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar)
    $relativeBuild = [Uri]::UnescapeDataString($toolsUri.MakeRelativeUri([Uri]$diagnosticBuild).ToString())
    $relativeOutput = [Uri]::UnescapeDataString($sourceUri.MakeRelativeUri([Uri]$diagnosticOutput).ToString())
    if ([IO.Path]::GetFullPath((Join-Path $source $relativeOutput)) -ne $diagnosticOutput -or
        [IO.Path]::GetFullPath((Join-Path $toolsDirectory $relativeBuild)) -ne $diagnosticBuild) {
        throw 'Diagnostic package fixture paths do not resolve inside staging.'
    }
    $diagnosticError = ''
    try {
        & (Join-Path $toolsDirectory 'package-v1.ps1') -Version $Version `
            -BuildDir $relativeBuild -OutputDir $relativeOutput | Out-Null
    } catch { $diagnosticError = $_.Exception.Message }
    if (Test-Path -LiteralPath $diagnosticOutput) {
        throw 'Diagnostic package rejection created release output.'
    }
    if (-not $diagnosticError.Contains('Audio-input recovery diagnostic')) {
        throw "Diagnostic build was not rejected by its marker: $diagnosticError"
    }
} finally {
    if (Test-Path -LiteralPath $stage) {
        $resolved = (Resolve-Path -LiteralPath $stage).Path
        if ((Split-Path $resolved -Parent) -ne $build -or
            (Split-Path $resolved -Leaf) -notmatch '^package-test-[0-9a-f]{32}$') {
            throw "Unsafe temporary package path; retained for inspection."
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

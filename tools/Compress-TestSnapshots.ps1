[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Join-Path $PSScriptRoot '..'),
    [string[]]$SnapshotDirectories = @(
        'outputs/readme-v2.1.0', 'outputs/readme-v2.1.1', 'outputs/readme-v2.1.3',
        'outputs/shortcut-edit-ui', 'outputs/ui-regression-20261004',
        'outputs/rounded-corners', 'outputs/settings-guide-alignment',
        'outputs/settings-shortcut-cards', 'outputs/settings-brand-centered',
        'outputs/settings-brand', 'outputs/40fps-option-ui',
        'outputs/40fps-fixed-ui', 'outputs/settings-disabled-text-audit'
    ),
    [switch]$Apply
)

# Lossless storage cleanup for historical, generated UI fixtures only. Extract
# each archive into the repository root to restore its original outputs/... paths.
# Current previews, comparisons, captures, settings, source and other file types
# are deliberately outside the allowlist. This script never deletes directories.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression

function Assert-SnapshotPath([string]$Path, [string]$Root, [switch]$CheckRootAncestors) {
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $base = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    if ($full -ne $base -and -not $full.StartsWith($base + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Snapshot path leaves the repository: $Path"
    }
    # Check existing ancestors as well, so a linked outputs/archive parent cannot
    # redirect an apparently contained path outside the repository.
    $ancestor = $full
    while ($ancestor) {
        if (Test-Path -LiteralPath $ancestor) {
            $item = Get-Item -LiteralPath $ancestor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse points are not allowed: $ancestor"
            }
        }
        if ($ancestor -eq $base -and -not $CheckRootAncestors) { break }
        $parent = [IO.Path]::GetDirectoryName($ancestor)
        if ($parent -eq $ancestor) { break }
        $ancestor = $parent
    }
    return $full
}

function Get-SnapshotHash([IO.Stream]$Stream) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Stream))).Replace('-', '') }
    finally { $sha.Dispose() }
}

function Get-SnapshotFileHash([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try { return Get-SnapshotHash $stream } finally { $stream.Dispose() }
}

function Get-SnapshotArchivePlan([string]$Root, [string[]]$Directories) {
    $allowed = @(
        'readme-v2.1.0', 'readme-v2.1.1', 'readme-v2.1.3', 'shortcut-edit-ui',
        'ui-regression-20261004', 'rounded-corners', 'settings-guide-alignment',
        'settings-shortcut-cards', 'settings-brand-centered', 'settings-brand',
        '40fps-option-ui', '40fps-fixed-ui', 'settings-disabled-text-audit',
        'settings-dropdown-spacing', 'settings-disabled-label', 'settings-text-rendering',
        'settings-themes', 'settings-medium', 'settings-slate-blue',
        'settings-neutral-blue', 'settings-charcoal', 'settings-polish',
        'settings-interaction', 'settings-monochrome', 'settings-refined',
        'settings-modern-snapshots'
    )
    $resolvedRoot = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\', '/')
    $null = Assert-SnapshotPath $resolvedRoot $resolvedRoot -CheckRootAncestors
    $outputs = Assert-SnapshotPath (Join-Path $resolvedRoot 'outputs') $resolvedRoot
    if (-not (Test-Path -LiteralPath $outputs -PathType Container)) { throw 'Missing outputs directory.' }
    $archiveRoot = Assert-SnapshotPath (Join-Path $outputs 'snapshot-archives') $resolvedRoot
    $seen = @{}
    foreach ($relative in $Directories) {
        if ($relative -notmatch '^outputs/([a-z0-9][a-z0-9.-]*)$' -or $Matches[1] -notin $allowed) {
            throw "Not an approved historical UI snapshot directory: $relative"
        }
        if ($seen.ContainsKey($relative)) { throw "Duplicate snapshot directory: $relative" }
        $seen[$relative] = $true
        $name = $Matches[1]
        $source = Assert-SnapshotPath (Join-Path $resolvedRoot $relative) $resolvedRoot
        if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "Missing directory: $relative" }
        $zip = Join-Path $archiveRoot ($name + '-bmp.zip')
        $manifest = Join-Path $archiveRoot ($name + '-bmp.manifest.json')
        foreach ($destination in @($zip, $manifest, ($zip + '.partial'), ($manifest + '.partial'))) {
            $null = Assert-SnapshotPath $destination $resolvedRoot
            if (Test-Path -LiteralPath $destination) { throw "Refusing to overwrite existing archive artifact: $destination" }
        }
        $files = [Collections.Generic.List[object]]::new()
        $pending = [Collections.Generic.Stack[string]]::new()
        $pending.Push($source)
        while ($pending.Count) {
            $directory = $pending.Pop()
            foreach ($item in Get-ChildItem -LiteralPath $directory -Force) {
                if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                    throw "Reparse points are not allowed: $($item.FullName)"
                }
                if ($item.PSIsContainer) { $pending.Push($item.FullName); continue }
                if ($item.Extension -ine '.bmp') { continue }
                # Every traversed directory and child was checked for reparse
                # points above; enumeration cannot escape the validated source.
                $files.Add([pscustomobject]@{
                    Path = $item.FullName
                    OriginalPath = $item.FullName.Substring($resolvedRoot.Length + 1).Replace('\', '/')
                    Bytes = [long]$item.Length
                    LastWriteTimeUtc = $item.LastWriteTimeUtc.ToString('o')
                    SHA256 = $null
                })
            }
        }
        if (-not $files.Count) { throw "No BMP snapshots found: $relative" }
        [pscustomobject]@{
            Repository = $resolvedRoot; Source = $source; RelativeDirectory = $relative
            ArchiveRoot = $archiveRoot; Archive = $zip; Manifest = $manifest
            Files = @($files | Sort-Object OriginalPath)
            OriginalBytes = [long](($files | Measure-Object Bytes -Sum).Sum)
        }
    }
}

function Assert-SnapshotSources($Plan) {
    $null = Assert-SnapshotPath $Plan.Source $Plan.Repository
    foreach ($file in $Plan.Files) {
        $null = Assert-SnapshotPath $file.Path $Plan.Source
        $item = Get-Item -LiteralPath $file.Path -Force
        if ($item.Length -ne $file.Bytes -or $item.LastWriteTimeUtc.ToString('o') -ne $file.LastWriteTimeUtc -or
            (Get-SnapshotFileHash $file.Path) -ne $file.SHA256) {
            throw "Source changed; original BMPs will not be removed: $($file.OriginalPath)"
        }
    }
}

function Test-SnapshotArchive($Plan, [string]$ArchivePath) {
    $null = Assert-SnapshotPath $ArchivePath $Plan.Repository
    $stream = [IO.File]::Open($ArchivePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Read, $true)
        try {
            if ($archive.Entries.Count -ne $Plan.Files.Count + 1) { throw 'Archive entry count mismatch.' }
            $manifestEntry = $archive.GetEntry('snapshot-manifest.json')
            if ($null -eq $manifestEntry) { throw 'Archive manifest is missing.' }
            $reader = [IO.StreamReader]::new($manifestEntry.Open())
            try { $embedded = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
            if (@($embedded.Files).Count -ne $Plan.Files.Count) { throw 'Archive manifest file count mismatch.' }
            foreach ($file in $Plan.Files) {
                $entry = $archive.GetEntry($file.OriginalPath)
                $record = @($embedded.Files | Where-Object { $_.OriginalPath -ceq $file.OriginalPath })
                if ($null -eq $entry -or $entry.Length -ne $file.Bytes -or $record.Count -ne 1 -or
                    $record[0].SHA256 -ne $file.SHA256 -or $record[0].Bytes -ne $file.Bytes) {
                    throw "Archive entry/manifest mismatch: $($file.OriginalPath)"
                }
                $entryStream = $entry.Open()
                try { $hash = Get-SnapshotHash $entryStream } finally { $entryStream.Dispose() }
                if ($hash -ne $file.SHA256) { throw "Archive SHA256 verification failed: $($file.OriginalPath)" }
            }
        } finally { $archive.Dispose() }
    } finally { $stream.Dispose() }
}

function New-SnapshotArchive($Plan) {
    $null = Assert-SnapshotPath $Plan.ArchiveRoot $Plan.Repository
    if (-not (Test-Path -LiteralPath $Plan.ArchiveRoot)) {
        New-Item -ItemType Directory -Path $Plan.ArchiveRoot | Out-Null
    }
    foreach ($file in $Plan.Files) {
        $null = Assert-SnapshotPath $file.Path $Plan.Source
        $file.SHA256 = Get-SnapshotFileHash $file.Path
    }
    Assert-SnapshotSources $Plan
    $manifest = [ordered]@{
        Format = 'LLCV historical UI BMP archive v1'
        CreatedUtc = [DateTime]::UtcNow.ToString('o')
        OriginalDirectory = $Plan.RelativeDirectory
        Restore = 'Extract this ZIP into the repository root to restore outputs/... paths.'
        OriginalBytes = $Plan.OriginalBytes
        Files = @($Plan.Files | Select-Object OriginalPath, Bytes, LastWriteTimeUtc, SHA256)
    }
    $json = $manifest | ConvertTo-Json -Depth 6
    # Failed creation leaves a partial ZIP for inspection, but no original can
    # be removed until the full archive and its sidecar have been finalized.
    $partial = $Plan.Archive + '.partial'
    $stream = [IO.File]::Open($partial, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
        try {
            foreach ($file in $Plan.Files) {
                $null = Assert-SnapshotPath $file.Path $Plan.Source
                $entry = $archive.CreateEntry($file.OriginalPath, [IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = [DateTimeOffset]::Parse($file.LastWriteTimeUtc)
                $source = [IO.File]::Open($file.Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
                try {
                    $target = $entry.Open()
                    try { $source.CopyTo($target) } finally { $target.Dispose() }
                } finally { $source.Dispose() }
            }
            $entry = $archive.CreateEntry('snapshot-manifest.json')
            $writer = [IO.StreamWriter]::new($entry.Open(), [Text.UTF8Encoding]::new($false))
            try { $writer.Write($json) } finally { $writer.Dispose() }
        } finally { $archive.Dispose() }
    } finally { $stream.Dispose() }
    Test-SnapshotArchive $Plan $partial
    Assert-SnapshotSources $Plan
    $manifest['ArchiveSHA256'] = Get-SnapshotFileHash $partial
    $manifest['ArchiveBytes'] = (Get-Item -LiteralPath $partial).Length
    $manifestStream = [IO.File]::Open(($Plan.Manifest + '.partial'), [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $writer = [IO.StreamWriter]::new($manifestStream, [Text.UTF8Encoding]::new($false))
        try { $writer.Write(($manifest | ConvertTo-Json -Depth 6)) } finally { $writer.Dispose() }
    } finally { $manifestStream.Dispose() }
    $null = Assert-SnapshotPath $Plan.Archive $Plan.Repository
    $null = Assert-SnapshotPath $Plan.Manifest $Plan.Repository
    # Two-argument File.Move never overwrites an existing destination.
    [IO.File]::Move($partial, $Plan.Archive)
    [IO.File]::Move(($Plan.Manifest + '.partial'), $Plan.Manifest)
}

function Remove-ArchivedSnapshots($Plan) {
    $null = Assert-SnapshotPath $Plan.Archive $Plan.Repository
    # Keep the verified archive locked against replacement/modification throughout
    # removal. Any verification failure occurs before the first original removal.
    $guard = [IO.File]::Open($Plan.Archive, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        Test-SnapshotArchive $Plan $Plan.Archive
        Assert-SnapshotSources $Plan
        foreach ($file in $Plan.Files) {
            $null = Assert-SnapshotPath $file.Path $Plan.Source
            $item = Get-Item -LiteralPath $file.Path -Force
            if ($item.Extension -ine '.bmp' -or $item.Length -ne $file.Bytes -or
                (Get-SnapshotFileHash $file.Path) -ne $file.SHA256) {
                throw "Source changed immediately before removal: $($file.OriginalPath)"
            }
            Remove-Item -LiteralPath $file.Path -ErrorAction Stop
        }
    } finally { $guard.Dispose() }
}

# Dot-sourcing exposes the same verification/removal functions to fixture tests
# without implicitly operating on the repository's real outputs.
if ($MyInvocation.InvocationName -eq '.') { return }
$plans = @(Get-SnapshotArchivePlan $RepositoryRoot $SnapshotDirectories)
if (-not $Apply) {
    foreach ($plan in $plans) {
        [pscustomobject]@{ Mode = 'DryRun'; Directory = $plan.RelativeDirectory
            BMPs = $plan.Files.Count; OriginalBytes = $plan.OriginalBytes; Archive = $plan.Archive }
    }
    Write-Host 'Dry run only: no files were created or removed. Use -Apply to create verified recovery archives.'
    return
}
$results = [Collections.Generic.List[object]]::new()
foreach ($plan in $plans) {
    Write-Host "Archiving $($plan.RelativeDirectory): $($plan.Files.Count) BMPs, $($plan.OriginalBytes) bytes."
    New-SnapshotArchive $plan
    Write-Host "ZIP verified. Rechecking originals and removing only archived BMPs: $($plan.RelativeDirectory)"
    Remove-ArchivedSnapshots $plan
    $storedBytes = [long](Get-Item -LiteralPath $plan.Archive).Length + [long](Get-Item -LiteralPath $plan.Manifest).Length
    $results.Add([pscustomobject]@{ Directory = $plan.RelativeDirectory; BMPs = $plan.Files.Count
        BeforeBytes = $plan.OriginalBytes; AfterArchiveAndManifestBytes = $storedBytes
        FreedBytes = $plan.OriginalBytes - $storedBytes; Archive = $plan.Archive; Manifest = $plan.Manifest })
    Write-Host "Recovered $($plan.OriginalBytes - $storedBytes) bytes; originals remain recoverable from the ZIP."
}
$report = [ordered]@{ CompletedUtc = [DateTime]::UtcNow.ToString('o'); Directories = @($results.ToArray())
    RemovedBMPs = ($results | Measure-Object BMPs -Sum).Sum
    BeforeBytes = ($results | Measure-Object BeforeBytes -Sum).Sum
    AfterArchiveAndManifestBytes = ($results | Measure-Object AfterArchiveAndManifestBytes -Sum).Sum
    FreedBytesExcludingThisReport = ($results | Measure-Object FreedBytes -Sum).Sum }
$reportPath = Join-Path $plans[0].ArchiveRoot ('compression-report-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '.json')
$null = Assert-SnapshotPath $reportPath $plans[0].Repository
$reportStream = [IO.File]::Open($reportPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    $writer = [IO.StreamWriter]::new($reportStream, [Text.UTF8Encoding]::new($false))
    try { $writer.Write(($report | ConvertTo-Json -Depth 6)) } finally { $writer.Dispose() }
} finally { $reportStream.Dispose() }
[pscustomobject]@{ Mode = 'Applied'; BMPs = $report.RemovedBMPs; BeforeBytes = $report.BeforeBytes
    AfterBytes = $report.AfterArchiveAndManifestBytes + (Get-Item -LiteralPath $reportPath).Length
    FreedBytes = $report.FreedBytesExcludingThisReport - (Get-Item -LiteralPath $reportPath).Length
    Report = $reportPath }

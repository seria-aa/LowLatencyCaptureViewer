$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$compress = Join-Path $repository 'tools/Compress-TestSnapshots.ps1'
. $compress
Add-Type -AssemblyName System.IO.Compression.FileSystem
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
$fixtureRoot = Join-Path $temporaryParent ('LLCV-SnapshotArchiveTests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
$script:checks = 0

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    ++$script:checks
}
function Assert-Failure([scriptblock]$Action, [string]$Expected) {
    $message = ''
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    Assert-True ($message.Contains($Expected)) "Expected failure '$Expected', received '$message'."
}
function New-Fixture([string]$Name) {
    $root = Join-Path $fixtureRoot $Name
    $folder = Join-Path $root 'outputs/readme-v2.1.0/nested'
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    # A valid, tiny 2x2, 24-bit BMP, generated entirely in the temporary fixture.
    $bmp = [byte[]]::new(70)
    $bmp[0] = 66; $bmp[1] = 77
    [BitConverter]::GetBytes([int]70).CopyTo($bmp, 2)
    [BitConverter]::GetBytes([int]54).CopyTo($bmp, 10)
    [BitConverter]::GetBytes([int]40).CopyTo($bmp, 14)
    [BitConverter]::GetBytes([int]2).CopyTo($bmp, 18)
    [BitConverter]::GetBytes([int]2).CopyTo($bmp, 22)
    [BitConverter]::GetBytes([int16]1).CopyTo($bmp, 26)
    [BitConverter]::GetBytes([int16]24).CopyTo($bmp, 28)
    [IO.File]::WriteAllBytes((Join-Path $folder 'test.bmp'), $bmp)
    $bmp[54] = 200
    [IO.File]::WriteAllBytes((Join-Path (Split-Path $folder -Parent) 'other.bmp'), $bmp)
    [IO.File]::WriteAllText((Join-Path $folder 'keep.txt'), 'Retain evidence and settings.')
    return $root
}

try {
    $root = New-Fixture 'dryrun'
    $dry = @(& $compress -RepositoryRoot $root -SnapshotDirectories 'outputs/readme-v2.1.0')
    Assert-True ($dry.Count -eq 1 -and $dry[0].Mode -eq 'DryRun' -and $dry[0].BMPs -eq 2) 'Dry run plan mismatch.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $root 'outputs/snapshot-archives'))) 'Dry run wrote an archive directory.'
    Assert-True (@(Get-ChildItem -LiteralPath $root -Recurse -File -Filter '*.bmp').Count -eq 2) 'Dry run removed originals.'
    foreach ($invalid in @('../outside', 'outputs/../src', '/outputs/readme-v2.1.0',
        'outputs/all-tabs-ui-20261025', 'outputs/cmaa-nis-validation', 'outputs/spatial-upscaling',
        'outputs/readme-v2.1.0/nested')) {
        Assert-Failure { Get-SnapshotArchivePlan $root @($invalid) } 'Not an approved'
    }
    Assert-Failure { Get-SnapshotArchivePlan $root @('outputs/readme-v2.1.0', 'outputs/readme-v2.1.0') } 'Duplicate snapshot'

    $expected = @{}
    foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -File -Filter '*.bmp') {
        $expected[$file.FullName.Substring($root.Length + 1).Replace('\', '/')] = Get-SnapshotFileHash $file.FullName
    }
    $result = & $compress -RepositoryRoot $root -SnapshotDirectories 'outputs/readme-v2.1.0' -Apply
    Assert-True ($result.Mode -eq 'Applied' -and $result.BMPs -eq 2) 'Apply result mismatch.'
    Assert-True (@(Get-ChildItem -LiteralPath $root -Recurse -File -Filter '*.bmp').Count -eq 0) 'Archived BMPs remain.'
    Assert-True (Test-Path -LiteralPath (Join-Path $root 'outputs/readme-v2.1.0/nested/keep.txt')) 'Non-BMP evidence was removed.'
    $zipPath = Join-Path $root 'outputs/snapshot-archives/readme-v2.1.0-bmp.zip'
    $archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        foreach ($entry in $archive.Entries) {
            if ($entry.FullName -eq 'snapshot-manifest.json') { continue }
            Assert-True ($expected.ContainsKey($entry.FullName)) 'Archive contains unexpected paths.'
            $stream = $entry.Open()
            try { $hash = Get-SnapshotHash $stream } finally { $stream.Dispose() }
            Assert-True ($hash -eq $expected[$entry.FullName]) 'Extracted bytes differ from originals.'
        }
    } finally { $archive.Dispose() }
    $extractRoot = Join-Path $fixtureRoot 'restored'
    [IO.Compression.ZipFile]::ExtractToDirectory($zipPath, $extractRoot)
    foreach ($relative in $expected.Keys) {
        Assert-True ((Get-SnapshotFileHash (Join-Path $extractRoot $relative)) -eq $expected[$relative]) 'Restored file checksum differs.'
    }
    Assert-Failure { & $compress -RepositoryRoot $root -SnapshotDirectories 'outputs/readme-v2.1.0' -Apply } 'Refusing to overwrite'

    $changedRoot = New-Fixture 'changed-source'
    $changedPlan = @(Get-SnapshotArchivePlan $changedRoot @('outputs/readme-v2.1.0'))[0]
    New-SnapshotArchive $changedPlan
    [IO.File]::WriteAllText($changedPlan.Files[0].Path, 'Changed after archive verification.')
    Assert-Failure { Remove-ArchivedSnapshots $changedPlan } 'Source changed'
    Assert-True (@(Get-ChildItem -LiteralPath $changedRoot -Recurse -File -Filter '*.bmp').Count -eq 2) 'Source-change failure removed originals.'

    $corruptRoot = New-Fixture 'corrupt-archive'
    $corruptPlan = @(Get-SnapshotArchivePlan $corruptRoot @('outputs/readme-v2.1.0'))[0]
    New-SnapshotArchive $corruptPlan
    $archive = [IO.Compression.ZipFile]::Open($corruptPlan.Archive, [IO.Compression.ZipArchiveMode]::Update)
    try {
        $entry = $archive.GetEntry($corruptPlan.Files[0].OriginalPath)
        $entry.Delete()
        $replacement = $archive.CreateEntry($corruptPlan.Files[0].OriginalPath)
        $stream = $replacement.Open()
        try { $stream.Write([byte[]]::new($corruptPlan.Files[0].Bytes), 0, $corruptPlan.Files[0].Bytes) }
        finally { $stream.Dispose() }
    } finally { $archive.Dispose() }
    Assert-Failure { Remove-ArchivedSnapshots $corruptPlan } 'SHA256 verification failed'
    Assert-True (@(Get-ChildItem -LiteralPath $corruptRoot -Recurse -File -Filter '*.bmp').Count -eq 2) 'Verification failure removed originals.'

    $linkedRoot = New-Fixture 'linked-source'
    # Simulate the OS reparse attribute without requiring junction-creation
    # privileges. The actual containment check must reject it before traversal.
    & {
        $mockReparsePath = Join-Path $linkedRoot 'outputs/readme-v2.1.0'
        function Get-Item {
            param([string]$LiteralPath, [switch]$Force)
            if ($LiteralPath -eq $mockReparsePath) {
                return [pscustomobject]@{ Attributes = [IO.FileAttributes]::ReparsePoint }
            }
            Microsoft.PowerShell.Management\Get-Item -LiteralPath $LiteralPath -Force:$Force
        }
        Assert-Failure { Get-SnapshotArchivePlan $linkedRoot @('outputs/readme-v2.1.0') } 'Reparse points'
    }
    Write-Host "TestSnapshotArchiveTests passed ($script:checks checks; only temporary fixtures modified)."
} finally {
    $resolvedFixture = (Resolve-Path -LiteralPath $fixtureRoot).Path
    if (-not $resolvedFixture.StartsWith($temporaryParent + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolvedFixture) -notmatch '^LLCV-SnapshotArchiveTests-[a-f0-9]{32}$') {
        throw "Refusing to remove an unexpected fixture path: $resolvedFixture"
    }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}

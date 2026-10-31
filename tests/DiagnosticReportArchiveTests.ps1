param([Parameter(Mandatory=$true)][string]$TestExe)
$ErrorActionPreference = 'Stop'
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('LLCV-report-archive-' + [char]0xd55c + [char]0xae00 + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
try {
    & $TestExe --fixture $fixtureRoot | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Report fixture failed' }
    $archives = @(Get-ChildItem -LiteralPath (Join-Path $fixtureRoot 'reports') -Filter '*.zip' -File)
    if ($archives.Count -ne 1) { throw 'Expected one finalized ZIP' }
    $archivePath = $archives[0].FullName
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        if ($archive.Entries.Count -ne 9) { throw 'Unexpected archive entry count' }
        foreach ($entry in $archive.Entries) {
            $reader = New-Object IO.StreamReader($entry.Open(), [Text.Encoding]::UTF8)
            try { $content = $reader.ReadToEnd() } finally { $reader.Dispose() }
            if ($content.Contains('PrivateName') -or $content.Contains('DO NOT INCLUDE')) { throw 'Privacy regression' }
            $unicodeExpected = [string]::Concat([char]0xd55c, [char]0xae00)
            if ($entry.FullName -eq 'settings.ini' -and !$content.Contains($unicodeExpected)) { throw 'Unicode regression' }
        }
    } finally { $archive.Dispose() }
    Write-Output 'PASS: independent .NET ZIP reader opened every entry and verified privacy/Unicode.'
} finally {
    # Only the unique directory created above; do not accept a caller-provided deletion target.
    if ([IO.Path]::GetFullPath($fixtureRoot).StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
    }
}

$ErrorActionPreference = "Stop"
$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$copyDocs = Join-Path $repository "tools/Copy-PackageDocs.ps1"
$checkDocs = Join-Path $repository "tools/Test-PackageDocs.ps1"
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
$fixtureRoot = Join-Path $temporaryParent ("LLCV-PackageDocsTests-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
$script:checks = 0

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    ++$script:checks
}
function Write-Fixture([string]$Root, [string]$Relative, [string]$Text) {
    $path = Join-Path $Root $Relative
    New-Item -ItemType Directory -Force -Path (Split-Path $path -Parent) | Out-Null
    [IO.File]::WriteAllText($path, $Text, [Text.UTF8Encoding]::new($false))
}
function Assert-BrokenLink([string]$Root, [string]$Text, [string]$Expected,
                           [string]$Relative = 'README.md') {
    Write-Fixture $Root $Relative $Text
    $errorText = ''
    try { & $checkDocs -PackageRoot $Root | Out-Null } catch { $errorText = $_.Exception.Message }
    Assert-True ($errorText.Contains($Expected)) "Expected '$Expected', received: $errorText"
}

try {
    # Exercise the actual portable documentation staging path without running
    # package-v1, building an executable, or modifying existing release assets.
    $package = Join-Path $fixtureRoot 'package'
    & $copyDocs -SourceRoot $repository -DestinationRoot $package
    $result = & $checkDocs -PackageRoot $package
    Assert-True ($result.Documents -gt 40) 'User guides/release history were not fully staged.'
    Assert-True ($result.LocalLinks -gt 100) 'Expected local documentation links were not checked.'
    foreach ($relative in @('LICENSE', 'DEPENDENCIES.txt', '실행안내.txt',
            'NIS-LICENSE.txt', 'CMAA2-LICENSE.txt', 'UPSCALING-NOTICE.txt',
            'docs/USAGE.md', 'docs/USAGE.ko.md', 'docs/VIDEO.md', 'docs/VIDEO.ko.md',
            'docs/AUDIO.md', 'docs/AUDIO.ko.md', 'docs/TROUBLESHOOTING.md',
            'docs/TROUBLESHOOTING.ko.md', 'docs/DIAGNOSTICS.md', 'docs/DIAGNOSTICS.ko.md')) {
        Assert-True (Test-Path -LiteralPath (Join-Path $package $relative) -PathType Leaf) "Missing $relative"
    }
    foreach ($relative in @('DESIGN.txt', 'BUILD_INFO.txt', 'GITHUB_PREP.md', 'tests',
            'docs/images/viewer.png', 'docs/rounded-corners-validation.md', 'docs/TEST-BLT.ko.md',
            'docs/vsr-all-formats-validation.md', 'docs/vsr-experiment.md', 'docs/vsr-hdr-validation.md',
            'docs/SPATIAL_UPSCALING_EXPERIMENT.md', 'docs/SPATIAL_UPSCALING_EXTENDED_COMPARISON.md',
            'docs/SPATIAL_UPSCALING_FHD_RECOMPARISON.md',
            'docs/UPSCALING_BOUNDARY_SAFETY.md', 'docs/UPSCALING_OVERFLOW_DIAGNOSTIC.md',
            'docs/UYVY_HDYC_SUPPORT.md', 'docs/RGB_CAPTURE_SUPPORT.md')) {
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $package $relative))) "Unnecessary package file: $relative"
    }
    $renamedSources = @{
        'NIS-LICENSE.txt' = 'third_party/upscaling/nis/licence.txt'
        'CMAA2-LICENSE.txt' = 'third_party/upscaling/cmaa2/license.txt'
        'UPSCALING-NOTICE.txt' = 'third_party/upscaling/NOTICE.txt'
    }
    foreach ($file in Get-ChildItem -LiteralPath $package -Recurse -File) {
        $relative = $file.FullName.Substring($package.Length + 1)
        $original = if ($renamedSources.ContainsKey($relative)) { $renamedSources[$relative] } else { $relative }
        Assert-True ((Get-FileHash -LiteralPath $file.FullName).Hash -eq
            (Get-FileHash -LiteralPath (Join-Path $repository $original)).Hash) "Document changed: $relative"
    }
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $package 'outputs'))) 'Local outputs must not be packaged.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $package 'src'))) 'Source code must not be packaged.'
    Assert-True (@(Get-ChildItem -LiteralPath $package -Recurse -File |
        Where-Object { $_.Extension -in @('.cpp', '.inl', '.exe', '.obj', '.ps1') }).Count -eq 0) 'Non-document test files were packaged.'

    # Original regression: an index may exist even when its guide/image targets
    # are absent. Move just fixture files, verify errors, then restore them.
    foreach ($relative in @('docs/USAGE.md', 'docs/USAGE.ko.md', 'docs/AUDIO.md',
                           'docs/images/help.png', 'docs/images/app-icon.png')) {
        $path = Join-Path $package $relative
        $held = Join-Path $fixtureRoot 'temporarily-held-document'
        Move-Item -LiteralPath $path -Destination $held
        try {
            $errorText = ''
            try { & $checkDocs -PackageRoot $package | Out-Null } catch { $errorText = $_.Exception.Message }
            Assert-True ($errorText.Contains([IO.Path]::GetFileName($relative))) "Omission not detected: $relative"
        } finally { Move-Item -LiteralPath $held -Destination $path }
    }

    $syntax = Join-Path $fixtureRoot 'syntax'
    Write-Fixture $syntax 'docs/guide.md' '# Guide'
    Write-Fixture $syntax 'docs/with space.md' '# Spaces'
    Write-Fixture $syntax 'docs/item(1).md' '# Parentheses'
    Write-Fixture $syntax 'docs/picture.png' 'image fixture'
    Write-Fixture $syntax 'README.md' @'
# Test
[guide](docs/guide.md#heading)
[space](<docs/with space.md> "title")
[encoded](docs/with%20space.md?raw=1#heading)
[parentheses](docs/item(1).md)
[escaped](docs/item\(1\).md)
[![image](docs/picture.png)](docs/guide.md)
[reference][guide]
[guide]: docs/guide.md "title"
<img src="docs/picture.png"><a href='docs/guide.md'>guide</a>
[site](https://example.invalid/unknown.md) [mail](mailto:example@example.invalid)
[cdn](//example.invalid/picture.png) [anchor](#heading)
`[inline example](missing-inline.md)`
``[inline example](missing-inline2.md)``
<!-- [comment](missing-comment.md) -->
```md
[example](missing-code.md)
```
~~~md
<img src="missing-code.png">
~~~~
'@
    $result = & $checkDocs -PackageRoot $syntax
    Assert-True ($result.LocalLinks -eq 10) "Link syntax coverage changed: $($result.LocalLinks)"
    Assert-BrokenLink $syntax '[missing](docs/no-such.md)' 'README.md:1: docs/no-such.md'
    Assert-BrokenLink $syntax '[missing](docs/no-such.md#heading)' 'file not packaged'
    Assert-BrokenLink $syntax '<img src="missing.png">' 'missing.png'
    Assert-BrokenLink $syntax "[ref]: docs/no-such.md" 'docs/no-such.md'
    Assert-BrokenLink $syntax '[outside](../elsewhere.md)' 'path leaves the package'
    Assert-BrokenLink $syntax '[outside](%2E%2E/elsewhere.md)' 'path leaves the package'
    Assert-BrokenLink $syntax '[absolute](C:\outside\guide.md)' 'absolute local path'
    Assert-BrokenLink $syntax '[absolute](file:///C:/outside/guide.md)' 'absolute local path'
    Assert-BrokenLink $syntax '[absolute](/outside/guide.md)' 'absolute local path'

    # A TXT launch note previously pointed at TEST-BLT.ko.md after that guide
    # was excluded from binary packages. Check bare paths as well as Markdown.
    $textSyntax = Join-Path $fixtureRoot 'text-syntax'
    Write-Fixture $textSyntax 'README.md' '# Readme'
    Write-Fixture $textSyntax 'docs/guide.md' '# Guide'
    Write-Fixture $textSyntax 'docs/with space.md' '# Spaces'
    Write-Fixture $textSyntax '실행안내.txt' @'
설명은 docs/guide.md를 보세요.
오디오 설명: docs\guide.md
자세한 안내: README.md
참조: docs/guide.md#heading
공백: <docs/with space.md>
공백: "docs/with space.md"
인코딩: docs/with%20space.md
인터넷: https://example.invalid/guide.md
소스 라이선스: third_party/asio/LICENSE.txt
'@
    $result = & $checkDocs -PackageRoot $textSyntax
    Assert-True ($result.LocalLinks -eq 7) "TXT guide syntax coverage changed: $($result.LocalLinks)"
    Assert-BrokenLink $textSyntax "첫 줄`n자세한 비교 방법은 docs/TEST-BLT.ko.md를 보세요." `
        '실행안내.txt:2: docs/TEST-BLT.ko.md (file not packaged)' '실행안내.txt'
    Assert-BrokenLink $textSyntax '설명: docs\missing.md' 'file not packaged' '실행안내.txt'
    Assert-BrokenLink $textSyntax '참조: ../outside.md' 'path leaves the package' '실행안내.txt'
    Assert-BrokenLink $textSyntax '참조: C:\outside\guide.md' 'absolute local path' '실행안내.txt'
    Assert-BrokenLink $textSyntax '참조: file:///C:/outside/guide.md' 'absolute local path' '실행안내.txt'

    $errorText = ''
    try { & $copyDocs -SourceRoot $repository -DestinationRoot $repository } catch { $errorText = $_.Exception.Message }
    Assert-True ($errorText.Contains('must not overwrite')) 'Source-tree overwrite was not rejected.'
    Write-Host "PackageDocsTests passed ($script:checks checks; no release output modified)."
} finally {
    $resolvedFixture = (Resolve-Path -LiteralPath $fixtureRoot).Path
    if (-not $resolvedFixture.StartsWith($temporaryParent + [IO.Path]::DirectorySeparatorChar,
                                         [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolvedFixture) -notmatch '^LLCV-PackageDocsTests-[a-f0-9]{32}$') {
        throw "Refusing to remove an unexpected fixture path: $resolvedFixture"
    }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}

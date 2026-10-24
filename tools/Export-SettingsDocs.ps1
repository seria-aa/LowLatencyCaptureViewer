param(
    [string]$BuildDir = "../build-v2160-release",
    [string]$OutputDir = "outputs/readme-v2.1.6"
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$build = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot $BuildDir)).Path
$output = Join-Path $root $OutputDir
New-Item -ItemType Directory -Force -Path $output | Out-Null
# Draw current native controls offscreen with fixture devices. No desktop,
# pointer, capture hardware, user preferences or installed app is accessed.
& (Join-Path $build "RecordingThemeTests.exe") --screenshots-only $output
if ($LASTEXITCODE -ne 0) { throw "Settings rendering failed." }
& (Join-Path $build "RecordingThemeTests.exe") --video-screenshots-only $output
if ($LASTEXITCODE -ne 0) { throw "Upscaling settings rendering failed." }
& (Join-Path $build "RecordingHelpTests.exe") $output
if ($LASTEXITCODE -ne 0) { throw "Help rendering failed." }
Add-Type -AssemblyName System.Drawing
foreach ($language in @("ko", "en")) {
    $suffix = if ($language -eq "en") { "-en" } else { "" }
    foreach ($entry in @(@("video", 0), @("audio", 1), @("recording", 2), @("shortcuts", 4))) {
        $source = Join-Path $output ("settings-dark-$language-tab" + $entry[1] + "-144dpi.bmp")
        if ($entry[0] -eq "video") {
            $source = Join-Path $output "settings-dark-$language-cmaa-nis-144dpi.bmp"
        }
        $target = Join-Path $root ("docs/images/settings-" + $entry[0] + $suffix + ".png")
        $bitmap = [System.Drawing.Bitmap]::new($source)
        try { $bitmap.Save($target, [System.Drawing.Imaging.ImageFormat]::Png) }
        finally { $bitmap.Dispose() }
    }
    Copy-Item -LiteralPath (Join-Path $output "help-dark-$language.png") -Destination (Join-Path $root "docs/images/help$suffix.png")
}

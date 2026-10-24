param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$PresentMon,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [switch]$Hdr
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new output directory.'}
if(Get-Process -Name obs64,LowLatencyCaptureViewer,LowLatencyCaptureViewer_Spatial -ErrorAction SilentlyContinue){throw 'Close other capture viewers first.'}
$profilePath=Join-Path $env:LOCALAPPDATA 'LowLatencyCaptureViewer/settings.ini'
$before=if(Test-Path -LiteralPath $profilePath){(Get-FileHash -LiteralPath $profilePath).Hash}else{'missing'}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
try {
    $orders=@(@('baseline','production-cmaa','vsr'),@('production-cmaa','vsr','baseline'),@('vsr','baseline','production-cmaa'))
    # HDR comparisons retain only the existing baseline/VSR paths.
    if($Hdr){$orders=@(@('baseline','vsr'),@('vsr','baseline'),@('baseline','vsr'))}
    for($round=0;$round -lt 3;$round++){
        $run=Join-Path $OutputDirectory ('round-'+$round)
        & "$PSScriptRoot/Compare-SpatialUpscaling.ps1" -BuildDirectory $BuildDirectory -PresentMon $PresentMon -OutputDirectory $run -Methods $orders[$round] -Hdr:$Hdr
        foreach($method in $orders[$round]){
            $log=Get-Content -LiteralPath (Join-Path $run "$method/app.log") -Raw
            $format=if($Hdr){'P010'}else{'NV12'}
            if($log -notmatch ("\[video-layout\] active parsed=1 actual="+$format+" 1920x1080 duration=16666[67]")){throw "Unexpected capture format: $method"}
            if($method -eq 'production-cmaa' -and $log -notmatch ("CMAA2 -> NIS active=1 HDR="+[int][bool]$Hdr+" sharpness=0.15")){throw 'Production filter not active'}
            if($log -match '\[upscaling\].*(unavailable|failed)'){throw 'Upscaling failure'}
        }
        & "$PSScriptRoot/Analyze-SpatialUpscaling.ps1" -Directory $run
    }
} finally {
    $after=if(Test-Path -LiteralPath $profilePath){(Get-FileHash -LiteralPath $profilePath).Hash}else{'missing'}
    [pscustomobject]@{Before=$before;After=$after;Equal=($before -eq $after)} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'settings-check.json') -Encoding utf8
    if($before -ne $after){Write-Warning 'User profile changed; not restoring it automatically.'}
}

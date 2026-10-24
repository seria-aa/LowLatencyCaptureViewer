param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$PresentMon,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new output directory.'}
if(Get-Process -Name obs64,LowLatencyCaptureViewer,LowLatencyCaptureViewer_Spatial -ErrorAction SilentlyContinue){throw 'Close other capture viewers before live measurements.'}
$profilePath=Join-Path $env:LOCALAPPDATA 'LowLatencyCaptureViewer/settings.ini'
$before=if(Test-Path -LiteralPath $profilePath){(Get-FileHash -LiteralPath $profilePath).Hash}else{'missing'}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
try {
    $orders=@(
        @('baseline','nis15','cmaa2-nis15'),
        @('nis15','cmaa2-nis15','baseline'),
        @('cmaa2-nis15','baseline','nis15')
    )
    for($round=0;$round -lt $orders.Count;$round++){
        $run=Join-Path $OutputDirectory ('round-'+$round)
        & "$PSScriptRoot/Compare-SpatialUpscaling.ps1" -BuildDirectory $BuildDirectory -PresentMon $PresentMon -OutputDirectory $run -Methods $orders[$round]
        foreach($method in $orders[$round]){
            $log=Get-Content -LiteralPath (Join-Path $run "$method/app.log") -Raw
            if($log -notmatch '\[video-layout\] active parsed=1 actual=NV12 1920x1080 duration=16666[67]'){throw "Unexpected capture format: $method"}
            if($method -eq 'cmaa2-nis15' -and $log -notmatch 'aa=6 sharpness=0.15'){throw 'CMAA2-before preset did not activate'}
            if($method -eq 'nis15' -and $log -notmatch 'aa=0 sharpness=0.15'){throw 'NIS 0.15 preset did not activate'}
        }
        & "$PSScriptRoot/Analyze-SpatialUpscaling.ps1" -Directory $run
    }
} finally {
    $after=if(Test-Path -LiteralPath $profilePath){(Get-FileHash -LiteralPath $profilePath).Hash}else{'missing'}
    if($after -ne $before){Write-Warning 'Settings changed during measurement; not restoring automatically.'}
    [pscustomobject]@{Before=$before;After=$after;Equal=($after -eq $before)} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'settings-check.json') -Encoding utf8
}

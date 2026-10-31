param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$PresentMon,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new measurement directory.'}
if(Get-Process -Name obs64,LowLatencyCaptureViewer,LowLatencyCaptureViewer_Spatial,LLCV_CmaaHighLatency,LLCV_CmaaPreserveLatency -ErrorAction SilentlyContinue){throw 'Close other capture viewers first.'}
$profilePaths=@((Join-Path $env:LOCALAPPDATA 'LowLatencyCaptureViewer/settings.ini'),(Join-Path $env:LOCALAPPDATA 'LowLatencyCaptureViewer/recording-prototype.ini'))
function ProfileHashes { @($profilePaths|ForEach-Object {if(Test-Path -LiteralPath $_){(Get-FileHash -LiteralPath $_).Hash}else{'missing'}}) }
$before=ProfileHashes
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$output=(Resolve-Path -LiteralPath $OutputDirectory).Path
$oldMode=$env:LLCV_SPATIAL_UPSCALER;$oldTrace=$env:LLCV_SPATIAL_TRACE
try {
    $orders=@(@('high','preserve'),@('preserve','high'),@('high','preserve'),@('preserve','high'))
    for($round=0;$round -lt $orders.Count;$round++) {
        $roundPath=Join-Path $output "round-$round";New-Item -ItemType Directory -Path $roundPath|Out-Null
        foreach($mode in $orders[$round]) {
            $file=if($mode -eq 'high'){'LLCV_CmaaHighLatency.exe'}else{'LLCV_CmaaPreserveLatency.exe'}
            $exe=Join-Path $BuildDirectory $file
            if(!(Test-Path -LiteralPath $exe)){throw 'Missing isolated timing viewer.'}
            $run=Join-Path $roundPath $mode;New-Item -ItemType Directory -Path $run|Out-Null
            $session='LLCV-CmaaPair-'+[guid]::NewGuid().ToString('N');$trace=$null;$viewer=$null
            try {
                $arguments=@('--process_name',$file,'--session_name',$session,'--timed','18','--terminate_after_timed',
                    '--no_console_stats','--no_track_input','--v1_metrics','--qpc_time','--output_file',('"'+(Join-Path $run 'presentmon.csv')+'"'))
                $trace=Start-Process -FilePath $PresentMon -ArgumentList $arguments -WindowStyle Hidden -PassThru `
                    -RedirectStandardOutput (Join-Path $run 'trace.log') -RedirectStandardError (Join-Path $run 'trace-errors.log')
                $env:LLCV_SPATIAL_UPSCALER='baseline';$env:LLCV_SPATIAL_TRACE=Join-Path $run 'frames.csv'
                $launchedAt=Get-Date
                # A visible video window is required for OS display-event timing.
                # Both EXEs compile TestProfile isolation; no real profile is read/imported.
                $viewer=Start-Process -FilePath $exe -ArgumentList @('--smoke-test','--spatial-live-test','yes','--spatial-reference','production-cmaa') `
                    -WorkingDirectory $BuildDirectory -WindowStyle Normal -PassThru
                Write-Output "Running round=$round mode=$mode PID=$($viewer.Id)"
                if(!$viewer.WaitForExit(30000)){throw 'Timing viewer did not close within 30 seconds.'}
                if(!$trace.WaitForExit(25000)){throw 'PresentMon did not finish.'}
                if($viewer.ExitCode -ne 0 -or $trace.ExitCode -ne 0){throw "Measurement exit viewer=$($viewer.ExitCode) trace=$($trace.ExitCode)"}
                $profile=Get-ChildItem -LiteralPath $env:TEMP -Directory -Filter "LLCV-TestProfile-$($viewer.Id)-*" |
                    Where-Object {$_.LastWriteTime -ge $launchedAt.AddSeconds(-1)} | Sort-Object LastWriteTime -Descending | Select-Object -First 1
                if(!$profile){throw 'Missing isolated test profile; never fall back to the user profile.'}
                Copy-Item -LiteralPath (Join-Path $profile.FullName 'logs/smoke-test.log') -Destination (Join-Path $run 'app.log')
                $log=Get-Content -LiteralPath (Join-Path $run 'app.log') -Raw
                if($log -notmatch '\[video-layout\] active parsed=1 actual=NV12 1920x1080 duration=16666[67]'){throw 'Unexpected input; expected FHD60 NV12 SDR.'}
                if($log -notmatch 'CMAA2 -> NIS active=1 HDR=0 sharpness=0.15'){throw 'Production upscaling inactive.'}
                if($log -notmatch '\[video-output\] create:.*size=2560x1440 fullscreen=0'){throw 'Unexpected output geometry.'}
                if($log -match '\[upscaling\].*(unavailable|failed)'){throw 'Upscaling failure.'}
                [pscustomobject]@{Mode=$mode;Executable=$file;SHA256=(Get-FileHash -LiteralPath $exe).Hash;PID=$viewer.Id;ExtraSharpness=[int]($mode -eq 'preserve')} |
                    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'variant.json') -Encoding utf8
                Write-Output "Finished round=$round mode=$mode"
            } finally {
                if($trace -and !$trace.HasExited){& $PresentMon --session_name $session --terminate_existing_session|Out-Null;$trace.WaitForExit(5000)|Out-Null}
                foreach($child in @($viewer,$trace)){if($child -and !$child.HasExited){$child.Kill();$child.WaitForExit(5000)|Out-Null}}
            }
        }
        & "$PSScriptRoot/../tools/Analyze-SpatialUpscaling.ps1" -Directory $roundPath
    }
} finally {
    $env:LLCV_SPATIAL_UPSCALER=$oldMode;$env:LLCV_SPATIAL_TRACE=$oldTrace
    $after=ProfileHashes
    $equal=($before.Count -eq $after.Count) -and !(Compare-Object $before $after)
    [pscustomobject]@{Before=$before;After=$after;Equal=$equal} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'settings-check.json') -Encoding utf8
    if(!$equal){Write-Warning 'User settings changed; not restoring automatically.'}
}

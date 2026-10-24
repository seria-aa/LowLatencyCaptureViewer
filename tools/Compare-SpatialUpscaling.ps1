param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][string]$PresentMon,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string[]]$Methods=@('baseline','bicubic','cas','fsr1','nis','sgsr'),
    [switch]$Hdr
)
$ErrorActionPreference='Stop'
if($Hdr -and @($Methods | Where-Object {$_ -notin @('baseline','vsr')}).Count){
    throw 'Spatial filters are SDR-only; HDR comparisons accept baseline and vsr only.'
}
$exe=Join-Path $BuildDirectory 'LowLatencyCaptureViewer_Spatial.exe'
if(!(Test-Path -LiteralPath $exe)){throw 'Build the separate spatial prototype first.'}
if(Test-Path -LiteralPath $OutputDirectory){throw 'Use a new output directory; existing measurements are preserved.'}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$output=(Resolve-Path -LiteralPath $OutputDirectory).Path
$oldMode=$env:LLCV_SPATIAL_UPSCALER;$oldTrace=$env:LLCV_SPATIAL_TRACE
try {
    foreach($method in $Methods) {
        if($method -notin @('baseline','bicubic','cas','fsr1','nis','nis15','cmaa2-nis15','production-cmaa','sgsr','ravu','fsrcnnx','jinc','lanczos','vsr')){throw 'Unknown scaler'}
        $run=Join-Path $output $method;New-Item -ItemType Directory -Path $run | Out-Null
        $session='LLCV-Spatial-'+[guid]::NewGuid().ToString('N')
        $trace=$null;$viewer=$null
        try {
            $args=@('--process_name','LowLatencyCaptureViewer_Spatial.exe','--session_name',$session,
                '--timed','18','--terminate_after_timed','--no_console_stats','--no_track_input',
                '--v1_metrics','--qpc_time','--output_file',('"'+(Join-Path $run 'presentmon.csv')+'"'))
            $trace=Start-Process -FilePath $PresentMon -ArgumentList $args -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput (Join-Path $run 'trace.log') -RedirectStandardError (Join-Path $run 'trace-errors.log')
            $env:LLCV_SPATIAL_UPSCALER=if($method -eq 'production-cmaa'){'baseline'}else{$method};$env:LLCV_SPATIAL_TRACE=Join-Path $run 'frames.csv'
            # Visible video is necessary for display-event timing. Smoke test
            # closes itself after 10 s; all setting overrides are session-only.
            $viewerArgs=@('--smoke-test','--spatial-live-test','yes','--spatial-reference',$method)
            if($Hdr){$viewerArgs+=@('--spatial-hdr','yes')}
            $launchedAt=Get-Date
            $viewer=Start-Process -FilePath $exe -ArgumentList $viewerArgs `
                -WorkingDirectory $BuildDirectory -WindowStyle Normal -PassThru
            Write-Output ('Running '+$method+' viewer='+$viewer.Id)
            if(!$viewer.WaitForExit(30000)){throw 'Prototype did not close within 30 seconds'}
            if(!$trace.WaitForExit(25000)){throw 'PresentMon did not finish'}
            if($viewer.ExitCode -ne 0 -or $trace.ExitCode -ne 0){throw ('Nonzero test exit: viewer='+$viewer.ExitCode+' trace='+$trace.ExitCode)}
            $profile=Get-ChildItem -LiteralPath $env:TEMP -Directory -Filter "LLCV-TestProfile-$($viewer.Id)-*" |
                Where-Object { $_.LastWriteTime -ge $launchedAt.AddSeconds(-1) } |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1
            if(!$profile){throw 'Missing isolated test profile; do not read the real app profile.'}
            $appLog=Join-Path $profile.FullName 'logs/smoke-test.log'
            if(!(Test-Path -LiteralPath $appLog)){throw 'Missing isolated test log.'}
            Copy-Item -LiteralPath $appLog -Destination (Join-Path $run 'app.log')
            Write-Output ('Finished '+$method)
        } finally {
            if($trace -and !$trace.HasExited){
                & $PresentMon --session_name $session --terminate_existing_session | Out-Null
                $trace.WaitForExit(5000)|Out-Null
            }
            # Only terminate child processes created by this test, never other apps.
            foreach($child in @($viewer,$trace)) {
                if($child -and !$child.HasExited){$child.Kill();$child.WaitForExit(5000)|Out-Null}
            }
        }
    }
} finally {
    $env:LLCV_SPATIAL_UPSCALER=$oldMode;$env:LLCV_SPATIAL_TRACE=$oldTrace
}

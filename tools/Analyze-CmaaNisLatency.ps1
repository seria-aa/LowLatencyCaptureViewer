param([Parameter(Mandatory=$true)][string]$GpuDirectory,
      [Parameter(Mandatory=$true)][string]$LiveDirectory)
$ErrorActionPreference='Stop'
function Average($values){[math]::Round(($values | Measure-Object -Average).Average,4)}
$gpu=@(Import-Csv -LiteralPath (Join-Path $GpuDirectory 'timings.csv'))
if($gpu.Count -ne 9){throw 'Expected three GPU runs of three methods.'}
$live=@(for($r=0;$r -lt 3;$r++){
    Import-Csv -LiteralPath (Join-Path $LiveDirectory "round-$r/summary.csv") |
        Select-Object *,@{Name='Round';Expression={$r}}
})
if($live.Count -ne 9){throw 'Expected three live runs of three methods.'}
$methods=@('baseline','nis15','cmaa2-nis15')
$summary=@(foreach($method in $methods){
    $g=@($gpu|Where-Object method -eq $method);$l=@($live|Where-Object Method -eq $method)
    if($g.Count -ne 3 -or $l.Count -ne 3){throw "Missing repeat: $method"}
    [pscustomobject]@{
        Method=$method
        GPUAverageMs=(Average $g.gpu_mean_ms)
        GPURunMinMs=[double]($g.gpu_mean_ms|Measure-Object -Minimum).Minimum
        GPURunMaxMs=[double]($g.gpu_mean_ms|Measure-Object -Maximum).Maximum
        DisplayAverageMs=(Average $l.CaptureToDisplayMeanMs)
        DisplayRunMinMs=[double]($l.CaptureToDisplayMeanMs|Measure-Object -Minimum).Minimum
        DisplayRunMaxMs=[double]($l.CaptureToDisplayMeanMs|Measure-Object -Maximum).Maximum
        RenderCompleteAverageMs=(Average $l.PresentToRenderCompleteMeanMs)
        Displayed=[int]($l.Displayed|Measure-Object -Sum).Sum
        Dropped=[int]($l.Dropped|Measure-Object -Sum).Sum
        Unmatched=[int]($l.Unmatched|Measure-Object -Sum).Sum
        PresentModes=@($l.PresentModes)
    }
})
$deltas=@(for($r=0;$r -lt 3;$r++){
    $l=@($live|Where-Object Round -eq $r)
    $b=[double]($l|Where-Object Method -eq 'baseline').CaptureToDisplayMeanMs
    $n=[double]($l|Where-Object Method -eq 'nis15').CaptureToDisplayMeanMs
    $a=[double]($l|Where-Object Method -eq 'cmaa2-nis15').CaptureToDisplayMeanMs
    [pscustomobject]@{Round=$r;CmaaVsNisMs=[math]::Round($a-$n,4);CmaaVsBaselineMs=[math]::Round($a-$b,4)}
})
$result=[pscustomobject]@{
    Definition='Equal-weight average of three run means. Live: capture callback to OS display event, not HDMI/optical latency.'
    Methods=$summary;PairedLiveDeltas=$deltas
}
$result|ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $LiveDirectory 'combined-summary.json') -Encoding utf8
$summary|Format-Table Method,GPUAverageMs,DisplayAverageMs,Displayed,Dropped -AutoSize
$deltas|Format-Table -AutoSize

param([Parameter(Mandatory=$true)][string]$Directory)
$ErrorActionPreference='Stop'
function Mean($values) {
    if(!$values.Count){return $null}
    return [math]::Round(($values|Measure-Object -Average).Average,4)
}
function Percentile($values,[double]$fraction) {
    if(!$values.Count){return $null}
    $sorted=@($values|Sort-Object);return [math]::Round($sorted[[int][math]::Floor(($sorted.Count-1)*$fraction)],4)
}
$results=@(foreach($run in Get-ChildItem -LiteralPath $Directory -Directory) {
    $frames=@(Import-Csv -LiteralPath (Join-Path $run.FullName 'frames.csv')|Sort-Object {[long]$_.present_qpc})
    $presents=@(Import-Csv -LiteralPath (Join-Path $run.FullName 'presentmon.csv')|Sort-Object {[long]$_.QPCTime})
    if($frames.Count -lt 180){throw "Insufficient live frames in $($run.Name)"}
    $frequency=[double]$frames[0].qpc_frequency
    $start=[long]$frames[0].present_qpc+3*$frequency
    $end=[long]$frames[-1].present_qpc-.5*$frequency
    $arrivalToPresent=@();$arrivalToDisplay=@();$presentToDisplay=@();$renderComplete=@()
    $modes=@{};$i=0;$dropped=0;$matched=0;$unmatched=0
    foreach($p in $presents) {
        $q=[long]$p.QPCTime;if($q -lt $start -or $q -gt $end){continue}
        while($i+1 -lt $frames.Count -and [long]$frames[$i+1].present_qpc -le $q){$i++}
        $f=$frames[$i];$delta=($q-[long]$f.present_qpc)*1000/$frequency
        if($delta -lt 0 -or $delta -gt .25){$unmatched++;continue}
        $matched++;$modes[$p.PresentMode]++
        $app=($q*1000000/$frequency-[double]$f.arrival_us)/1000
        $arrivalToPresent+=$app
        if([int]$p.Dropped -ne 0){$dropped++;continue}
        if([double]$p.msUntilDisplayed -le 0){continue}
        $presentToDisplay+=[double]$p.msUntilDisplayed
        $arrivalToDisplay+=($app+[double]$p.msUntilDisplayed)
        $renderComplete+=[double]$p.msUntilRenderComplete
    }
    if($matched -lt 120 -or $unmatched -gt $matched*.01){throw "Invalid trace match for $($run.Name): $matched matched/$unmatched unmatched"}
    if($arrivalToDisplay.Count -lt 100){throw "Insufficient actual display events for $($run.Name)"}
    [pscustomobject]@{
        Method=$run.Name;Matched=$matched;Unmatched=$unmatched;Dropped=$dropped;Displayed=$arrivalToDisplay.Count
        CaptureToPresentMeanMs=(Mean $arrivalToPresent)
        CaptureToDisplayMeanMs=(Mean $arrivalToDisplay)
        CaptureToDisplayMedianMs=(Percentile $arrivalToDisplay .5)
        CaptureToDisplayP95Ms=(Percentile $arrivalToDisplay .95)
        PresentToDisplayMeanMs=(Mean $presentToDisplay)
        PresentToRenderCompleteMeanMs=(Mean $renderComplete)
        PresentModes=($modes|ConvertTo-Json -Compress)
    }
})
$results|Export-Csv -LiteralPath (Join-Path $Directory 'summary.csv') -NoTypeInformation -Encoding utf8
$results|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $Directory 'summary.json') -Encoding utf8
$results|Format-Table Method,Matched,Dropped,CaptureToPresentMeanMs,CaptureToDisplayMeanMs,CaptureToDisplayP95Ms -AutoSize
Write-Output 'Capture callback to OS display event, not HDMI/optical latency. First 3 seconds and final 0.5 second excluded.'

param([Parameter(Mandatory)][string]$Directory,[int]$MinimumSeconds=3600)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$Directory=[IO.Path]::GetFullPath($Directory)
$result=Get-Content -LiteralPath (Join-Path $Directory 'result.json') -Raw|ConvertFrom-Json
$rows=@(Import-Csv -LiteralPath (Join-Path $Directory 'samples.csv'))
if(!$result.passed -or $result.elapsedSeconds -lt $MinimumSeconds -or $result.operations -lt 500 -or $result.completedCycles -lt 100){throw 'Soak result did not complete the required duration and operations'}
if($rows.Count -lt $MinimumSeconds*.8){throw 'Insufficient resource/progress samples'}
$lastTime=0.;$lastFrames=0L;$lastLoops=0L;$frameAdvancedAt=0.;$loopAdvancedAt=0.;$maxFrameGap=0.;$maxLoopGap=0.;$maxSampleGap=0.
$cpuSum=0.;$maxPrivate=0L;$maxHandles=0;$maxThreads=0
foreach($row in $rows){
 $time=[double]::Parse($row.elapsed_seconds,[Globalization.CultureInfo]::InvariantCulture)
 $frames=[long]$row.frames;$loops=[long]$row.loops
 if($time -le $lastTime -or $frames -lt $lastFrames -or $loops -lt $lastLoops){throw 'Nonmonotonic time/frame/loop samples'}
 $gap=$time-$lastTime;$maxSampleGap=[Math]::Max($gap,$maxSampleGap)
 if($gap -gt 10){throw "Sampling gap exceeds 10 seconds at $time"}
 $maxFrameGap=[Math]::Max($maxFrameGap,$time-$frameAdvancedAt)
 $maxLoopGap=[Math]::Max($maxLoopGap,$time-$loopAdvancedAt)
 if($frames -gt $lastFrames){$frameAdvancedAt=$time}
 if($loops -gt $lastLoops){$loopAdvancedAt=$time}
 if($time-$frameAdvancedAt -gt 10){throw "Playback frame did not advance for over 10 seconds at $time"}
 if($time-$loopAdvancedAt -gt 20){throw "Local fixture did not finish/reopen for over 20 seconds at $time"}
 if([int]$row.state -notin @(1,2,5)){throw "Unexpected playback state $($row.state) at $time"}
 $cpuSum += [double]::Parse($row.cpu_percent_machine,[Globalization.CultureInfo]::InvariantCulture)*$gap
 $maxPrivate=[Math]::Max($maxPrivate,[long]$row.private_bytes)
 $maxHandles=[Math]::Max($maxHandles,[int]$row.handles);$maxThreads=[Math]::Max($maxThreads,[int]$row.threads)
 $lastTime=$time;$lastFrames=$frames;$lastLoops=$loops
}
if($lastTime -lt $MinimumSeconds-1 -or $result.elapsedSeconds-$lastTime -gt 2){throw 'Samples do not cover the full test duration'}
if($lastFrames -ne $result.framesObserved -or $lastLoops -ne $result.completedLoops){throw 'Final frame/loop accounting mismatch'}
$report=[ordered]@{passed=$true;minimumSeconds=$MinimumSeconds;sampledSeconds=$lastTime;sampleCount=$rows.Count;
 continuousFrameProgress=$true;continuousLoopProgress=$true;maxFrameProgressGapSeconds=$maxFrameGap;
 maxLoopProgressGapSeconds=$maxLoopGap;maxSampleGapSeconds=$maxSampleGap;frames=$lastFrames;loops=$lastLoops;
 observedFramesPerSecond=$lastFrames/$lastTime;weightedCpuPercentMachine=$cpuSum/$lastTime;
 peakSampledPrivateBytes=$maxPrivate;peakSampledHandles=$maxHandles;peakSampledThreads=$maxThreads;
 samplesSHA256=(Get-FileHash -LiteralPath (Join-Path $Directory 'samples.csv') -Algorithm SHA256).Hash;
 resultSHA256=(Get-FileHash -LiteralPath (Join-Path $Directory 'result.json') -Algorithm SHA256).Hash;
 verifiedUtc=[DateTime]::UtcNow.ToString('o')}
$report|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $Directory 'trajectory-verification.json') -Encoding UTF8
[pscustomobject]$report

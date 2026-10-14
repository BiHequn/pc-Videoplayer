param([Parameter(Mandatory)][string]$Player,[Parameter(Mandatory)][string]$DemoServer,
 [Parameter(Mandatory)][string]$Fixtures,[Parameter(Mandatory)][string]$OutputDirectory,[switch]$DiscardCheckpoint)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
$OutputDirectory=Join-Path $OutputDirectory ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
[IO.Directory]::CreateDirectory($OutputDirectory)|Out-Null
function Run-Player([string]$Action) {
 $info=New-Object Diagnostics.ProcessStartInfo
 $info.FileName=[IO.Path]::GetFullPath($Player)
 $info.Arguments='--decoder ffmpeg --data-root "'+$OutputDirectory+'\data" --history-source "'+$source+'" --history-action '+$Action+' --history-smoke "'+$OutputDirectory+'\'+$Action+'"'
 $info.UseShellExecute=$false;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
 $child=[Diagnostics.Process]::Start($info)
 $stdout=$child.StandardOutput.ReadToEndAsync();$stderr=$child.StandardError.ReadToEndAsync()
 try {
  if(!$child.WaitForExit(45000)){$child.Kill();$child.WaitForExit();throw "Desktop $Action timed out"}
  [IO.File]::WriteAllText((Join-Path $OutputDirectory "$Action.stdout.log"),$stdout.Result)
  [IO.File]::WriteAllText((Join-Path $OutputDirectory "$Action.stderr.log"),$stderr.Result)
  if($child.ExitCode -ne 0){throw "Desktop $Action exit $($child.ExitCode)"}
  $report=Get-Content -LiteralPath (Join-Path $OutputDirectory "$Action/result.json") -Raw|ConvertFrom-Json
  if(!$report.passed){throw "Desktop $Action report failed"}
  return $report
 } finally {$child.Dispose()}
}
$serverInfo=New-Object Diagnostics.ProcessStartInfo
$serverInfo.FileName=[IO.Path]::GetFullPath($DemoServer)
$requestLog=Join-Path $OutputDirectory 'requests.jsonl'
$serverInfo.Arguments='"'+[IO.Path]::GetFullPath($Fixtures)+'" 0 "'+$requestLog+'"'
$serverInfo.UseShellExecute=$false;$serverInfo.CreateNoWindow=$true
$serverInfo.RedirectStandardOutput=$true;$serverInfo.RedirectStandardError=$true
$server=[Diagnostics.Process]::Start($serverInfo)
try {
 $line=$server.StandardOutput.ReadLineAsync()
 if(!$line.Wait(10000)-or$line.Result -notmatch '^Demo FLV: (http://127\.0\.0\.1:[0-9]+/stream\.flv)$'){throw 'Owned demo server did not become ready'}
 $source=$Matches[1]
 $seed=Run-Player 'seed'
 if($seed.downloaded -le 0 -or $seed.downloaded -ge $seed.total){throw 'Seed did not retain partial download'}
 $sha=[Security.Cryptography.SHA256]::Create()
 try {$key=([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($source)))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
 $checkpointPath=Join-Path $OutputDirectory "data/cache/$key/state.json"
 $checkpoint=Get-Content -LiteralPath $checkpointPath -Raw|ConvertFrom-Json
 $blocks=@($checkpoint.resources[0].blocks)
 if(!$blocks.Count){throw 'No durable block checkpoint'}
 Copy-Item -LiteralPath $checkpointPath -Destination (Join-Path $OutputDirectory 'seed-cache-checkpoint.json')
 # A second signed source shares its redacted display address while its task
 # identity differs. Cleanup must keep that record and its cache sentinel.
 $sha=[Security.Cryptography.SHA256]::Create()
 try {$siblingKey=([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($source+'?signature=second-task')))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
 $statePath=Join-Path $OutputDirectory 'data/desktop-state.json'
 $seedState=Get-Content -LiteralPath $statePath -Raw|ConvertFrom-Json
 $seedState.downloads=@($seedState.downloads)+@([pscustomobject]@{source=$source;total=1;downloaded=0;state='paused';requiresSource=$true;taskKey=$siblingKey})
 $seedState|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $statePath -Encoding UTF8
 $siblingDirectory=Join-Path $OutputDirectory "data/cache/$siblingKey"
 [IO.Directory]::CreateDirectory($siblingDirectory)|Out-Null
 [IO.File]::WriteAllText((Join-Path $siblingDirectory 'keep'),'other task sentinel')
 if($DiscardCheckpoint){Move-Item -LiteralPath $checkpointPath -Destination (Join-Path $OutputDirectory 'discarded-checkpoint.json')}
 $requestCount=@(Get-Content -LiteralPath $requestLog).Count
 $resume=Run-Player 'resume'
 $requests=@(Get-Content -LiteralPath $requestLog|Select-Object -Skip $requestCount|ForEach-Object{$_|ConvertFrom-Json}|Where-Object{$_.path -eq '/stream.flv' -and $_.range -ne 'bytes=0-0'})
 # Frame arrival can coincide with all active blocks finishing. Such a seed
 # legitimately has complete blocks and no partial block; prove their reuse
 # by requiring every new Range to start at its durable checkpoint instead.
 $completeBlocks=0
 for($index=0;$index -lt $blocks.Count;++$index){
  $end=[Math]::Min([long]$checkpoint.resources[0].length,([long]$index+1)*524288)
  if([long]$blocks[$index] -eq $end){++$completeBlocks}
 }
 $reusedPartial=$false
 foreach($request in $requests){
  if($request.range -notmatch '^bytes=([0-9]+)-([0-9]+)$'){throw 'Unexpected resumed Range'}
  $start=[long]$Matches[1];$block=[int][Math]::Floor($start/524288)
  if($block -ge $blocks.Count -or $start -ne [long]$blocks[$block]){throw "Resumed Range $start did not start from durable checkpoint"}
  if($start -gt $block*524288){$reusedPartial=$true}
 }
 $reused=$requests.Count -gt 0 -and ($reusedPartial -or $completeBlocks -gt 0)
 if(!$reused){throw 'No HTTP request proved reuse of durable partial or complete blocks'}
 if($resume.restoredBytes -ne $seed.downloaded -or !$resume.cleaned){throw 'Resume or cleanup evidence mismatch'}
 $state=Get-Content -LiteralPath (Join-Path $OutputDirectory 'data/desktop-state.json') -Raw|ConvertFrom-Json
 if(@($state.downloads|Where-Object taskKey -eq $key).Count){throw 'Deleted download was resurrected on final save'}
 if(@($state.downloads|Where-Object taskKey -eq $siblingKey).Count -ne 1 -or !(Test-Path -LiteralPath (Join-Path $siblingDirectory 'keep'))){throw 'Cleanup removed another task sharing the redacted source'}
 if(Test-Path -LiteralPath (Join-Path $OutputDirectory "data/cache/$key")){throw 'Task cache directory survived cleanup'}
 [ordered]@{passed=$true;seed=$seed;resume=$resume;source=$source;reusedCheckpoint=$reused;reusedPartialCheckpoint=$reusedPartial;completeBlocksSkipped=$completeBlocks;resumedRanges=$requests;finishedUtc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding UTF8
 Write-Output "PASS desktop history, process restart, resume and cache cleanup: $OutputDirectory"
}finally{
 if(!$server.HasExited){$server.Kill();$server.WaitForExit()}
 $server.Dispose()
}

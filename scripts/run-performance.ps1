[CmdletBinding()]
param(
    [string]$BuildDirectory = '',
    [string]$OutputDirectory = '',
    [ValidateRange(3,6)][int]$Seconds = 4,
    [ValidateRange(1,60)][int]$Fps = 30,
    [ValidateRange(1,100)][int]$Repeats = 3,
    [string]$BackgroundNote = '',
    [ValidatePattern('^$|^\d+(,\d+)*$')][string]$IdleMSBuildIgnore = '',
    [switch]$PreflightOnly,
    [switch]$Smoke,
    [switch]$Visible,
    [switch]$ReuseFixtures,
    [switch]$Resume,
    [switch]$ChartsOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$BuildDirectory) { $BuildDirectory = Join-Path $PSScriptRoot '../build/desktop/Release' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot '../build/third-layer/benchmark-results' }
$invariant = [Globalization.CultureInfo]::InvariantCulture
function Number([double]$Value) { $Value.ToString('0.###', $invariant) }
function Escape([string]$Value) { [System.Net.WebUtility]::HtmlEncode($Value) }
function Write-Utf8([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}
function Median($Values) {
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { throw 'Cannot compute median of empty samples' }
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if ($sorted.Count % 2) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle-1] + [double]$sorted[$middle]) / 2
}
function Total($Rows,[string]$Property) {
    $sum=0.0
    foreach ($row in $Rows) { $sum += [double]$row.$Property }
    return $sum
}
$idleNodeIds = @()
if ($IdleMSBuildIgnore) { $idleNodeIds = @($IdleMSBuildIgnore.Split(',') | ForEach-Object { [int]$_ } | Sort-Object -Unique) }
$buildPreflightChecks = [Collections.Generic.List[object]]::new()
function Read-BuildCensus {
    $processes = @(Get-Process -Name cl,link,MSBuild,cmake,ninja -ErrorAction SilentlyContinue)
    $blocked = @($processes | Where-Object { $_.ProcessName -ne 'MSBuild' -or $_.Id -notin $idleNodeIds })
    if ($blocked.Count) {
        $description = ($blocked | ForEach-Object { "$($_.ProcessName) PID $($_.Id)" }) -join ', '
        throw "Formal measurement deferred: build process present: $description. Idle-node exceptions never apply to build controllers or compiler/linker processes."
    }
    $nodes = @($processes | Where-Object ProcessName -eq 'MSBuild')
    if (!$nodes.Count) { return [pscustomobject]@{ nodes=@(); census=@() } }
    try { $census = @(Get-CimInstance Win32_Process -ErrorAction Stop | Select-Object ProcessId,ParentProcessId,CreationDate,Name,ExecutablePath,CommandLine) }
    catch { throw "Cannot prove reusable MSBuild nodes idle: CIM process inspection failed: $($_.Exception.Message). Wait for node expiry or run preflight with process-query access." }
    foreach ($node in $nodes) {
        $info = @($census | Where-Object ProcessId -eq $node.Id)
        if ($info.Count -ne 1) { throw "Cannot identify MSBuild PID $($node.Id) in process census; retry after it exits." }
        # Accept only the known reusable worker startup grammar; targets, project
        # paths, additional switches and non-reusable controllers fail closed.
        $commandPattern = '^\s*(?:"[^"\r\n]*[\\/]MSBuild\.exe"|[^\s"]*[\\/]MSBuild\.exe)\s+/nologo\s+/nodemode:1\s+/nodeReuse:true(?:\s+/low:false)?\s*$'
        if ([string]$info[0].CommandLine -notmatch $commandPattern) {
            throw "MSBuild PID $($node.Id) is not an explicitly reusable /nodemode:1 worker; command-line exception rejected."
        }
        if (@($census | Where-Object ProcessId -eq $info[0].ParentProcessId).Count) {
            throw "MSBuild PID $($node.Id) has a live parent PID $($info[0].ParentProcessId); cannot prove detached idle node."
        }
        $children = @($census | Where-Object ParentProcessId -eq $node.Id)
        foreach ($child in $children) {
            $expectedHost = Join-Path $env:SystemRoot 'System32/conhost.exe'
            if ($child.Name -ne 'conhost.exe' -or $child.ExecutablePath -ne $expectedHost -or
                [string]$child.CommandLine -notmatch '^\\\?\?\\[A-Za-z]:\\WINDOWS\\system32\\conhost\.exe\s+0x[0-9a-f]+\s*$' -or
                [Math]::Abs(($child.CreationDate-$info[0].CreationDate).TotalSeconds) -gt 2 -or
                @($census | Where-Object ParentProcessId -eq $child.ProcessId).Count) {
                throw "MSBuild PID $($node.Id) has a live child other than its original leaf System32 console host; cannot prove idle node."
            }
        }
        try {
            $node.Refresh(); $threads = @($node.Threads)
            if (!$threads.Count -or @($threads | Where-Object { $_.ThreadState -ne [Diagnostics.ThreadState]::Wait }).Count) {
                throw 'one or more threads are not waiting'
            }
        } catch { throw "MSBuild PID $($node.Id) idle-thread inspection failed: $($_.Exception.Message)" }
    }
    return [pscustomobject]@{ nodes=$nodes; census=$census }
}
function Require-IdleBuilds {
    $started = [DateTime]::UtcNow
    $initial = Read-BuildCensus
    $evidence = @{}
    foreach ($node in $initial.nodes) {
        $info = @($initial.census | Where-Object ProcessId -eq $node.Id)[0]
        $evidence[$node.Id] = [ordered]@{
            pid=$node.Id; command_line=$info.CommandLine; parent_pid=$info.ParentProcessId
            parent_exited=$true; no_build_children=$true; all_sampled_threads_waiting=$true
            creation_utc=$info.CreationDate.ToUniversalTime().ToString('o')
            cpu_start_seconds=$node.TotalProcessorTime.TotalSeconds; cpu_end_seconds=$node.TotalProcessorTime.TotalSeconds
            exited_during_check=$false; console_hosts=@()
        }
        foreach ($child in @($initial.census | Where-Object ParentProcessId -eq $node.Id)) {
            $hostProcess=Get-Process -Id $child.ProcessId -ErrorAction Stop
            $evidence[$node.Id].console_hosts += [ordered]@{
                pid=$child.ProcessId; executable_path=$child.ExecutablePath; command_line=$child.CommandLine
                creation_utc=$child.CreationDate.ToUniversalTime().ToString('o')
                cpu_start_seconds=$hostProcess.TotalProcessorTime.TotalSeconds; cpu_end_seconds=$hostProcess.TotalProcessorTime.TotalSeconds
            }
        }
    }
    if ($initial.nodes.Count) {
        # Two one-second observation intervals. A zero CPU delta alone is not
        # sufficient: command, process tree and worker thread states must agree.
        for ($sample=0; $sample -lt 2; $sample++) {
            Start-Sleep -Milliseconds 1000
            $current = Read-BuildCensus
            foreach ($node in $current.nodes) {
                if (!$evidence.ContainsKey($node.Id)) { throw "New MSBuild PID $($node.Id) appeared during idle verification; retry after builds finish." }
                $info = @($current.census | Where-Object ProcessId -eq $node.Id)[0]
                $record = $evidence[$node.Id]
                if ($info.CreationDate.ToUniversalTime().ToString('o') -ne $record.creation_utc) { throw "MSBuild PID $($node.Id) was reused during verification." }
                $record.cpu_end_seconds=$node.TotalProcessorTime.TotalSeconds
                if ($record.cpu_end_seconds -ne $record.cpu_start_seconds) { throw "MSBuild PID $($node.Id) consumed CPU during observation; not proven idle." }
                foreach ($child in @($current.census | Where-Object ParentProcessId -eq $node.Id)) {
                    $hostRecord=@($record.console_hosts | Where-Object { $_.pid -eq $child.ProcessId })
                    if ($hostRecord.Count -ne 1 -or $hostRecord[0].creation_utc -ne $child.CreationDate.ToUniversalTime().ToString('o')) {
                        throw "MSBuild PID $($node.Id) console host identity changed during idle verification."
                    }
                    $hostProcess=Get-Process -Id $child.ProcessId -ErrorAction Stop
                    $hostRecord[0].cpu_end_seconds=$hostProcess.TotalProcessorTime.TotalSeconds
                    if ($hostRecord[0].cpu_end_seconds -ne $hostRecord[0].cpu_start_seconds) { throw "MSBuild PID $($node.Id) console host consumed CPU; not proven idle." }
                }
            }
            foreach ($nodeId in @($evidence.Keys)) {
                if ($nodeId -notin @($current.nodes | ForEach-Object Id)) { $evidence[$nodeId].exited_during_check=$true }
            }
        }
    }
    $check = [ordered]@{
        checked_start_utc=$started.ToString('o'); checked_end_utc=[DateTime]::UtcNow.ToString('o')
        explicit_requested_idle_node_pids=$idleNodeIds; observed_idle_nodes=@($evidence.Values)
        policy='Only explicitly listed detached reusable /nodemode:1 /nodeReuse:true workers with all sampled threads waiting and zero CPU delta across two one-second intervals are permitted. Child exceptions are restricted to original leaf System32 console hosts with verified identity and zero CPU delta. Actual build controllers/compiler/linker processes remain blocked. Snapshot checks cannot prevent subsequent builds; parent must keep builds stopped.'
    }
    $buildPreflightChecks.Add($check)
    if ($initial.nodes.Count) { Write-Host "Proven idle reusable MSBuild nodes retained: $((@($evidence.Keys) | Sort-Object) -join ', '); CPU delta 0, parent exited, all sampled threads waiting." }
}
function Wait-IdleBuilds {
    $deadline=[DateTime]::UtcNow.AddSeconds(45)
    for (;;) {
        try { Require-IdleBuilds; return }
        catch {
            $reason=$_.Exception.Message
            # Retry only sampled activity of the explicit nodes/console hosts.
            # Actual builds, unknown processes, failed identity/CIM checks stop.
            if ($reason -notmatch '(consumed CPU|idle-thread inspection failed)' -or [DateTime]::UtcNow -ge $deadline) { throw }
            $buildPreflightChecks.Add([ordered]@{ checked_end_utc=[DateTime]::UtcNow.ToString('o'); deferred_reason=$reason; measurement_started=$false })
            Write-Host "Waiting for explicitly listed node inactivity: $reason"
            Start-Sleep -Milliseconds 2000
        }
    }
}
function Get-RecordedProperty($Object,[string]$Name) {
    $property=$Object.PSObject.Properties[$Name]
    if ($property) { return $property.Value }
    return $null
}
function Comparison-Gate($Group,$Metadata) {
    $reasons=[Collections.Generic.List[string]]::new()
    $yuv=@($Group | Where-Object mode -eq 'native-yuv'); $bgra=@($Group | Where-Object mode -eq 'bgra')
    if ($yuv.Count -lt 3 -or $bgra.Count -lt 3) { $reasons.Add('At least three observations per mode are required.') }
    if ($yuv.Count -ne $bgra.Count) { $reasons.Add('Mode observation counts differ.') }
    foreach ($modeRows in @(@{items=$yuv},@{items=$bgra})) {
        $items=@($modeRows.items)
        if (@($items | Select-Object -ExpandProperty repeat -Unique).Count -ne $items.Count) { $reasons.Add('Duplicate repeat identifiers.') }
    }
    $sameFields=@('source_sha256','benchmark_sha256','source_width','source_height','source_fps','source_frames','source_duration_seconds',
        'source_bytes','source_codec','source_pixel_format','source_container','source_audio_streams','renderer','decoder',
        'gl_vendor','gl_renderer','gl_version','window_width','window_height','hidden_window','swap_interval','swap_interval_disable_succeeded')
    foreach ($field in $sameFields) {
        $values=@($Group | ForEach-Object { Get-RecordedProperty $_ $field })
        if ($values.Count -ne @($Group).Count -or @($values | Where-Object { $null -eq $_ -or "$_" -eq '' }).Count) {
            $reasons.Add("Missing comparison evidence: $field.")
        } elseif (@($values | Select-Object -Unique).Count -ne 1) { $reasons.Add("Comparison values differ: $field.") }
    }
    foreach ($run in $Group) {
        $label="$($run.mode) repeat $($run.repeat)"
        if ($run.status -ne 'passed') { $reasons.Add("$label did not pass playback.") }
        if ($run.source_audio_streams -ne 0 -or $run.decoder -ne 'ffmpeg' -or $run.renderer -ne 'OpenGLRenderer') { $reasons.Add("$label does not match the video-only FFmpeg/OpenGL contract.") }
        if ($run.observed_pixel_format -ne $(if ($run.mode -eq 'native-yuv') {'yuv420p'} else {'bgra'})) { $reasons.Add("$label pixel format differs from requested mode.") }
        if ($run.source_frames -le 0 -or $run.unique_delivered_frames -ne $run.source_frames -or
            $run.session_published_frames -ne $run.source_frames -or $run.session_dropped_frames -ne 0 -or
            $run.observer_missed_frames -ne 0 -or $run.unaccounted_source_frames -ne 0 -or $run.frames.Count -ne $run.source_frames) {
            $reasons.Add("$label fails complete-frame gate (zero drops/misses/unaccounted frames required).")
        }
        if (Get-RecordedProperty $run 'source_hash_changed_during_run') { $reasons.Add("$label source hash changed during playback.") }
        if ($run.swap_interval -eq -999) { $reasons.Add("$label swap interval unavailable.") }
    }
    if ($Metadata.completed -ne $true) { $reasons.Add('Measurement session is incomplete.') }
    return [pscustomobject]@{ comparable=($reasons.Count -eq 0); reasons=@($reasons.ToArray()); required_session_drops=0; required_observer_misses=0; required_unaccounted_frames=0 }
}
function Chart($Rows, [string]$Metric, [string]$Title, [string]$Unit, [string]$Path) {
    $groups = @($Rows | Group-Object source_width | Sort-Object { [int]$_.Name })
    $max = [double](($Rows | Measure-Object -Property $Metric -Maximum).Maximum)
    if ($max -le 0) { $max = 1 }
    $scale = 275 / $max
    $svg = [Text.StringBuilder]::new()
    [void]$svg.AppendLine('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 900 420" role="img">')
    [void]$svg.AppendLine("<title>$(Escape $Title)</title><rect width='900' height='420' fill='#f6f8fc'/><g font-family='Segoe UI,Arial,sans-serif' fill='#1c2638'>")
    [void]$svg.AppendLine("<text x='35' y='30' font-size='20'>$(Escape $Title)</text><text x='35' y='53' font-size='12'>Bars: median; dots: individual runs. Unit: $(Escape $Unit)</text>")
    for ($tick=0; $tick -le 4; $tick++) {
        $value = $max * $tick / 4; $y = 335 - $value * $scale
        [void]$svg.AppendLine("<line x1='75' x2='860' y1='$(Number $y)' y2='$(Number $y)' stroke='#dbe2ed'/><text x='68' y='$(Number ($y+4))' text-anchor='end' font-size='12'>$(Number $value)</text>")
    }
    $slot = 780 / $groups.Count
    for ($i=0; $i -lt $groups.Count; $i++) {
        $group = $groups[$i]; $cx = 75 + $slot * ($i+.5)
        $example = $group.Group[0]
        [void]$svg.AppendLine("<text x='$(Number $cx)' y='359' text-anchor='middle' font-size='14'>$(Escape ([string]$example.resolution)) ($($example.source_width)×$($example.source_height))</text>")
        $modes = @('native-yuv','bgra')
        for ($m=0; $m -lt $modes.Count; $m++) {
            $points = @($group.Group | Where-Object mode -eq $modes[$m] | ForEach-Object { [double]$_.$Metric })
            if (!$points.Count) { continue }
            $median = Median $points; $x = $cx - 58 + $m * 65
            $height = $median * $scale; $color = if ($m -eq 0) { '#2678c9' } else { '#dd8640' }
            [void]$svg.AppendLine("<rect x='$(Number $x)' y='$(Number (335-$height))' width='50' height='$(Number $height)' fill='$color'/><text x='$(Number ($x+25))' y='$(Number (325-$height))' text-anchor='middle' font-size='12'>$(Number $median)</text>")
            for ($p=0; $p -lt $points.Count; $p++) {
                $px = $x + 10 + ($p % 4)*9; $py = 335 - $points[$p]*$scale
                [void]$svg.AppendLine("<circle cx='$(Number $px)' cy='$(Number $py)' r='3' fill='#1c2638'><title>$(Escape $modes[$m]) run $($p+1): $(Number $points[$p]) $(Escape $Unit)</title></circle>")
            }
        }
    }
    [void]$svg.AppendLine("<rect x='285' y='385' width='14' height='14' fill='#2678c9'/><text x='307' y='397' font-size='13'>native YUV</text><rect x='460' y='385' width='14' height='14' fill='#dd8640'/><text x='482' y='397' font-size='13'>CPU BGRA</text></g></svg>")
    Write-Utf8 $Path $svg.ToString()
}

$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
[void][IO.Directory]::CreateDirectory($OutputDirectory)
$metadataFile = Join-Path $OutputDirectory 'environment.json'
$runs = [Collections.Generic.List[object]]::new()
if ($PreflightOnly) {
    Require-IdleBuilds
    Write-Utf8 (Join-Path $OutputDirectory 'build-preflight.json') ([ordered]@{ schema_version=1; measurement_started=$false; checks=@($buildPreflightChecks.ToArray()) } | ConvertTo-Json -Depth 12)
    Write-Host 'Build preflight passed; no fixture generation or benchmark process started.'
    return
}
if ($ChartsOnly) {
    $data = Get-Content -LiteralPath (Join-Path $OutputDirectory 'results.json') -Raw | ConvertFrom-Json
    foreach ($run in $data.runs) { $runs.Add($run) }
    $metadata = Get-Content -LiteralPath $metadataFile -Raw | ConvertFrom-Json
} else {
    if (!$Smoke -and $Repeats -lt 3) { throw 'Formal benchmark requires at least 3 repeats; use -Smoke for quick validation.' }
    if ($Resume -and !$ReuseFixtures) { throw 'Resume requires -ReuseFixtures.' }
    if (!$Resume -and (Test-Path -LiteralPath (Join-Path $OutputDirectory 'results.json'))) {
        throw 'Existing observations present; use -Resume -ReuseFixtures or a new output directory to preserve evidence.'
    }
    $BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
    $generator = Join-Path $BuildDirectory 'make-benchmark-media.exe'
    $benchmark = Join-Path $BuildDirectory 'player-benchmark.exe'
    foreach ($exe in @($generator,$benchmark)) { if (!(Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Missing executable: $exe" } }
    # Read process evidence; never stop unrelated build/test processes.
    if (!$Smoke) {
        Wait-IdleBuilds
    }
    $fixtureDirectory = Join-Path $OutputDirectory 'fixtures'
    $rawDirectory = Join-Path $OutputDirectory 'raw'
    [void][IO.Directory]::CreateDirectory($fixtureDirectory)
    [void][IO.Directory]::CreateDirectory($rawDirectory)
    $resolutions = if ($Smoke) { @('720p') } else { @('720p','1080p','4k') }
    if ($Smoke) { $Seconds=3; $Fps=10; $Repeats=1 }
    $generateArgs = @($fixtureDirectory,'--seconds',[string]$Seconds,'--fps',[string]$Fps)
    if ($Smoke) { $generateArgs += @('--only','720p') }
    if (!$ReuseFixtures) {
        & $generator @generateArgs
        if ($LASTEXITCODE -ne 0) { throw "Fixture generation failed: $LASTEXITCODE" }
    }
    $cpu=@(); $gpu=@(); $environmentErrors=@(); $osCaption=[Environment]::OSVersion.VersionString
    $osVersion=[Environment]::OSVersion.Version.ToString(); $osBuild=[Environment]::OSVersion.Version.Build; $totalMemory=$null
    try { $cpu = @(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors) }
    catch { $environmentErrors += "Win32_Processor: $($_.Exception.Message)" }
    try {
        $os = Get-CimInstance Win32_OperatingSystem
        $osCaption=$os.Caption; $osVersion=$os.Version; $osBuild=$os.BuildNumber; $totalMemory=[double]$os.TotalVisibleMemorySize*1024
    } catch { $environmentErrors += "Win32_OperatingSystem: $($_.Exception.Message)" }
    try { $gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion) }
    catch { $environmentErrors += "Win32_VideoController: $($_.Exception.Message)" }
    $backgroundPlayers = @(Get-Process -Name player-soak-test,native-player,player-benchmark -ErrorAction SilentlyContinue | Select-Object Id,ProcessName)
    $metadata = [ordered]@{
        schema_version=1; purpose=$(if ($Smoke) {'smoke; not performance evidence'} else {'serial performance measurement'})
        started_utc=[DateTime]::UtcNow.ToString('o'); ended_utc=$null; completed=$false
        cpu=$cpu; gpu=$gpu; os_caption=$osCaption; os_version=$osVersion; os_build=$osBuild
        processor_identifier=$env:PROCESSOR_IDENTIFIER; environment_probe_errors=$environmentErrors
        measurement_caveats=@('No control over unrelated background workloads. Record concurrent soak/build activity before interpreting results.','GPU identification comes from each actual GL context; WMI fields may be unavailable.')
        background_note=$BackgroundNote; background_player_processes_at_start=$backgroundPlayers
        idle_msbuild_ignore_requested=$idleNodeIds; build_preflight_checks=@($buildPreflightChecks.ToArray())
        idle_msbuild_environment_note=$(if ($idleNodeIds.Count) { "Explicit reusable-node exception requested for PIDs $($idleNodeIds -join ', '); each remaining node and original console host is independently verified before every playback. Observation evidence is recorded in build_preflight_checks. These idle processes remain resident; snapshot checks do not guarantee future inactivity." } else { 'No MSBuild idle-node exceptions requested.' })
        total_physical_memory_bytes=$totalMemory
        benchmark_executable=$benchmark; benchmark_sha256=(Get-FileHash -LiteralPath $benchmark -Algorithm SHA256).Hash
        generator_sha256=(Get-FileHash -LiteralPath $generator -Algorithm SHA256).Hash
        repeats=$Repeats; requested_seconds=$Seconds; requested_fps=$Fps
        visible_window=[bool]$Visible; output_directory=$OutputDirectory; fixtures=@()
    }
    foreach ($resolution in $resolutions) {
        $fixture = Join-Path $fixtureDirectory "$resolution.avi"
        if (!(Test-Path -LiteralPath $fixture -PathType Leaf)) { throw "Missing fixture $fixture" }
        $metadata.fixtures += [ordered]@{ resolution=$resolution; path=$fixture; sha256=(Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash; bytes=(Get-Item -LiteralPath $fixture).Length }
    }
    if ($Resume) {
        $previousMetadata=Get-Content -LiteralPath $metadataFile -Raw | ConvertFrom-Json
        $previousData=Get-Content -LiteralPath (Join-Path $OutputDirectory 'results.json') -Raw | ConvertFrom-Json
        if ($previousMetadata.benchmark_sha256 -ne $metadata.benchmark_sha256 -or $previousMetadata.repeats -ne $Repeats -or
            $previousMetadata.requested_seconds -ne $Seconds -or $previousMetadata.requested_fps -ne $Fps -or
            $previousMetadata.visible_window -ne [bool]$Visible) { throw 'Resume contract or benchmark executable identity changed.' }
        foreach ($fixture in $metadata.fixtures) {
            $previousFixture=@($previousMetadata.fixtures | Where-Object resolution -eq $fixture.resolution)
            if ($previousFixture.Count -ne 1 -or $previousFixture[0].sha256 -ne $fixture.sha256) { throw "Resume fixture hash changed: $($fixture.resolution)" }
        }
        $metadata.started_utc=$previousMetadata.started_utc
        $metadata.resume_utc=[DateTime]::UtcNow.ToString('o')
        $newChecks=@($buildPreflightChecks.ToArray())
        $buildPreflightChecks.Clear()
        foreach ($check in $previousMetadata.build_preflight_checks) { $buildPreflightChecks.Add($check) }
        foreach ($check in $newChecks) { $buildPreflightChecks.Add($check) }
        $metadata.build_preflight_checks=@($buildPreflightChecks.ToArray())
        foreach ($run in $previousData.runs) {
            $fixture=@($metadata.fixtures | Where-Object resolution -eq $run.resolution)[0]
            if (!(Get-RecordedProperty $run 'source_sha256')) {
                $run | Add-Member -NotePropertyName source_sha256 -NotePropertyValue $fixture.sha256
                $run | Add-Member -NotePropertyName source_hash_evidence -NotePropertyValue 'Fixture hashed before original serial session and verified unchanged on resume.'
                $run | Add-Member -NotePropertyName source_hash_changed_during_run -NotePropertyValue $false
            }
            if (!(Get-RecordedProperty $run 'benchmark_sha256')) { $run | Add-Member -NotePropertyName benchmark_sha256 -NotePropertyValue $metadata.benchmark_sha256 }
            $runs.Add($run)
        }
        Write-Host "Resuming $($runs.Count) preserved observations; executable and fixture hashes unchanged."
    }
    Write-Utf8 $metadataFile ($metadata | ConvertTo-Json -Depth 12)
    $runOrder=0
    try {
      for ($repeat=1; $repeat -le $Repeats; $repeat++) {
        # Alternate which mode goes first; reverse resolution order on even rounds.
        $roundResolutions = @($resolutions)
        if ($repeat % 2 -eq 0) { [Array]::Reverse($roundResolutions) }
        foreach ($resolution in $roundResolutions) {
            $modes = if ($repeat % 2) { @('native-yuv','bgra') } else { @('bgra','native-yuv') }
            foreach ($mode in $modes) {
                $runOrder++
                if (@($runs.ToArray() | Where-Object { $_.resolution -eq $resolution -and $_.mode -eq $mode -and $_.repeat -eq $repeat }).Count) { continue }
                if (!$Smoke) {
                    Wait-IdleBuilds
                    $metadata.build_preflight_checks=@($buildPreflightChecks.ToArray())
                    Write-Utf8 $metadataFile ($metadata | ConvertTo-Json -Depth 12)
                }
                $rawFile = Join-Path $rawDirectory "$resolution-$mode-$repeat.json"
                $fixture = Join-Path $fixtureDirectory "$resolution.avi"
                $sourceHashBefore=(Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash
                if ((Get-FileHash -LiteralPath $benchmark -Algorithm SHA256).Hash -ne $metadata.benchmark_sha256) { throw 'Benchmark executable changed during the session.' }
                $arguments = @($fixture,$rawFile,$mode,'--repeat',[string]$repeat,'--timeout','30')
                if ($Visible) { $arguments += '--visible' }
                if ($Smoke) { $arguments += @('--screenshot',(Join-Path $OutputDirectory "$mode-smoke.png")) }
                & $benchmark @arguments
                if ($LASTEXITCODE -ne 0) { throw "Benchmark failed ($LASTEXITCODE): $rawFile" }
                $run = Get-Content -LiteralPath $rawFile -Raw | ConvertFrom-Json
                if ($run.status -ne 'passed') { throw "Failed run recorded: $rawFile" }
                $sourceHashAfter=(Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash
                $run | Add-Member -NotePropertyName source_sha256 -NotePropertyValue $sourceHashBefore
                $run | Add-Member -NotePropertyName source_hash_evidence -NotePropertyValue 'SHA256 immediately before and after this playback.'
                $run | Add-Member -NotePropertyName source_hash_changed_during_run -NotePropertyValue ($sourceHashBefore -ne $sourceHashAfter)
                $run | Add-Member -NotePropertyName benchmark_sha256 -NotePropertyValue $metadata.benchmark_sha256
                $run | Add-Member -NotePropertyName resolution -NotePropertyValue $resolution
                $run | Add-Member -NotePropertyName run_order -NotePropertyValue $runOrder
                $run | Add-Member -NotePropertyName raw_json -NotePropertyValue $rawFile
                $run | Add-Member -NotePropertyName cpu_seconds_per_delivered_frame -NotePropertyValue ($run.process_cpu_seconds / $run.unique_delivered_frames)
                $run | Add-Member -NotePropertyName peak_rss_mib -NotePropertyValue ($run.peak_rss_bytes / 1MB)
                $run | Add-Member -NotePropertyName peak_private_commit_mib -NotePropertyValue ($run.peak_private_commit_bytes / 1MB)
                $runs.Add($run)
                # Checkpoint keeps completed observations available if a later run fails.
                Write-Utf8 (Join-Path $OutputDirectory 'results.json') ([ordered]@{ schema_version=1; runs=@($runs.ToArray()) } | ConvertTo-Json -Depth 12)
            }
        }
      }
    } catch {
        $metadata.ended_utc=[DateTime]::UtcNow.ToString('o')
        $metadata.completed=$false; $metadata.stopped_reason=$_.Exception.Message
        $metadata.build_preflight_checks=@($buildPreflightChecks.ToArray())
        Write-Utf8 $metadataFile ($metadata | ConvertTo-Json -Depth 12)
        throw
    }
    $metadata.ended_utc=[DateTime]::UtcNow.ToString('o'); $metadata.completed=$true
    Write-Utf8 $metadataFile ($metadata | ConvertTo-Json -Depth 12)
}
if (!$runs.Count) { throw 'No observations available' }
$rows = @($runs.ToArray() | Select-Object -Property * -ExcludeProperty frames,memory_samples)
$rows | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'results.csv') -NoTypeInformation -Encoding UTF8
$summary = [Collections.Generic.List[object]]::new()
foreach ($group in ($rows | Group-Object resolution | Sort-Object { [int]$_.Group[0].source_width })) {
    $yuv = @($group.Group | Where-Object mode -eq 'native-yuv')
    $bgra = @($group.Group | Where-Object mode -eq 'bgra')
    $a = if ($yuv.Count) { Median @($yuv.process_cpu_seconds) } else { $null }
    $b = if ($bgra.Count) { Median @($bgra.process_cpu_seconds) } else { $null }
    $gate=Comparison-Gate @($runs.ToArray() | Where-Object resolution -eq $group.Name) $metadata
    if ($b -le 0) { $gate.comparable=$false; $gate.reasons += 'BGRA CPU median must be positive.' }
    $change = if ($gate.comparable) { 100*($b-$a)/$b } else { $null }
    $summary.Add([pscustomobject][ordered]@{
        resolution=$group.Name; native_repeats=$yuv.Count; bgra_repeats=$bgra.Count
        comparison_status=$(if ($gate.comparable) {'comparable observations'} else {'inconclusive'})
        comparison_reasons=($gate.reasons -join ' '); comparison_gate=$gate
        native_cpu_median_seconds=$a; bgra_cpu_median_seconds=$b; native_cpu_reduction_percent=$change
        native_rss_median_mib=$(if ($yuv.Count) { Median @($yuv.peak_rss_mib) } else { $null }); bgra_rss_median_mib=$(if ($bgra.Count) { Median @($bgra.peak_rss_mib) } else { $null })
        native_delivered_frames_median=$(if ($yuv.Count) { Median @($yuv.unique_delivered_frames) } else { $null }); bgra_delivered_frames_median=$(if ($bgra.Count) { Median @($bgra.unique_delivered_frames) } else { $null })
        native_drop_total=(Total $yuv 'session_dropped_frames')
        bgra_drop_total=(Total $bgra 'session_dropped_frames')
        native_observer_missed_total=(Total $yuv 'observer_missed_frames')
        bgra_observer_missed_total=(Total $bgra 'observer_missed_frames')
    })
}
Write-Utf8 (Join-Path $OutputDirectory 'summary.json') ($summary.ToArray() | ConvertTo-Json -Depth 6)
$summary.ToArray() | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'summary.csv') -NoTypeInformation -Encoding UTF8
$charts = @(
    @('process_cpu_seconds','Process CPU per playback','seconds','cpu.svg'),
    @('cpu_core_equivalents','Average CPU core equivalents','cores','cores.svg'),
    @('peak_rss_mib','Peak process working set','MiB','memory.svg'),
    @('peak_private_commit_mib','Peak process private commit','MiB','private.svg'),
    @('first_frame_ms','Open to first presented frame','ms','first-frame.svg'),
    @('delivered_fps','Unique presented frames / total elapsed','frames/s','fps.svg'),
    @('sync_abs_p95_ms','Absolute session sync difference p95','ms','sync.svg'),
    @('render_complete_p95_ms','Update through GPU completion p95','ms','render.svg')
)
foreach ($chart in $charts) { Chart $rows $chart[0] $chart[1] $chart[2] (Join-Path $OutputDirectory $chart[3]) }
$html = [Text.StringBuilder]::new()
[void]$html.AppendLine('<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>Native player performance observations</title><style>body{max-width:1100px;margin:35px auto;padding:0 22px;font:16px/1.5 Segoe UI,Arial;color:#1c2638;background:#fff}img{width:100%}table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}td,th{padding:8px;border-bottom:1px solid #ddd;text-align:right}td:first-child,th:first-child{text-align:left}.charts{display:grid;grid-template-columns:repeat(auto-fit,minmax(390px,1fr));gap:14px}</style><h1>Actual playback observations</h1>')
[void]$html.AppendLine("<p>$(Escape ([string]$metadata.purpose)). $($rows.Count) fresh-process runs; $(Escape ([string]$metadata.started_utc)) to $(Escape ([string]$metadata.ended_utc)). FFmpeg software decode and the same OpenGLRenderer; video-only deterministic MPEG4 fixtures. Fixed 1280×720 $(if ($metadata.visible_window) {'visible'} else {'hidden'}) window; source-resolution offscreen video texture; glFinish after SwapBuffers.</p>")
if ($metadata.PSObject.Properties['background_note'] -and $metadata.background_note) {
    [void]$html.AppendLine("<p>Background workload: $(Escape ([string]$metadata.background_note))</p>")
} elseif ($metadata -is [Collections.IDictionary] -and $metadata.Contains('background_note') -and $metadata.background_note) {
    [void]$html.AppendLine("<p>Background workload: $(Escape ([string]$metadata.background_note))</p>")
}
if ($metadata.PSObject.Properties['idle_msbuild_environment_note']) {
    [void]$html.AppendLine("<p>Build preflight: $(Escape ([string]$metadata.idle_msbuild_environment_note))</p>")
} elseif ($metadata -is [Collections.IDictionary] -and $metadata.Contains('idle_msbuild_environment_note')) {
    [void]$html.AppendLine("<p>Build preflight: $(Escape ([string]$metadata.idle_msbuild_environment_note))</p>")
}
[void]$html.AppendLine('<p>CPU covers session construction/open/playback/cleanup. RSS/private commit peaks include process setup. Source probe and WGL/ImGui initialization precede CPU timing. Sync is a sampled session video-clock difference; no audio stream is present. Hidden-window swaps do not measure monitor/display latency. Repeats are observations on this host, not significance estimates.</p><p>Comparison requires at least three runs per mode, identical source SHA256 and measured properties, executable identity, actual GL/GPU/window/swap configuration, full source frame delivery and zero session drops, observer misses or unaccounted frames. Failed or missing evidence marks the comparison inconclusive; observation charts remain available.</p><p><a href="results.json">Raw aggregate JSON</a> · <a href="results.csv">Run CSV</a> · <a href="environment.json">Environment and hashes</a> · <a href="summary.csv">Summary CSV</a></p><table><tr><th>Resolution</th><th>YUV CPU median, s</th><th>BGRA CPU median, s</th><th>YUV CPU reduction / gate</th><th>Frames YUV/BGRA</th><th>Session drops YUV/BGRA</th><th>Observer misses YUV/BGRA</th></tr>')
foreach ($s in $summary) {
    $comparisonText=if ($s.comparison_gate.comparable) { "$(Number $s.native_cpu_reduction_percent)%" } else { "inconclusive: $(Escape $s.comparison_reasons)" }
    $yuvCpuText=if ($null -ne $s.native_cpu_median_seconds) { Number $s.native_cpu_median_seconds } else { 'unobserved' }
    $bgraCpuText=if ($null -ne $s.bgra_cpu_median_seconds) { Number $s.bgra_cpu_median_seconds } else { 'unobserved' }
    $yuvFramesText=if ($null -ne $s.native_delivered_frames_median) { Number $s.native_delivered_frames_median } else { 'unobserved' }
    $bgraFramesText=if ($null -ne $s.bgra_delivered_frames_median) { Number $s.bgra_delivered_frames_median } else { 'unobserved' }
    [void]$html.AppendLine("<tr><td>$(Escape $s.resolution)</td><td>$yuvCpuText</td><td>$bgraCpuText</td><td>$comparisonText</td><td>$yuvFramesText / $bgraFramesText</td><td>$($s.native_drop_total) / $($s.bgra_drop_total)</td><td>$($s.native_observer_missed_total) / $($s.bgra_observer_missed_total)</td></tr>")
    if ($s.comparison_gate.comparable) {
        $direction = if ($s.native_cpu_reduction_percent -gt 0) {'lower'} elseif ($s.native_cpu_reduction_percent -lt 0) {'higher'} else {'equal'}
        Write-Host "Comparable observations $($s.resolution): native YUV median CPU is $direction; signed reduction=$(Number $s.native_cpu_reduction_percent)% (positive=lower CPU). Background limitations still apply."
    } else { Write-Host "Inconclusive $($s.resolution): $($s.comparison_reasons) Observation values retained; no reduction claim." }
}
[void]$html.AppendLine('</table><div class="charts">')
foreach ($chart in $charts) { [void]$html.AppendLine("<img src='$(Escape $chart[3])' alt='$(Escape $chart[1])'>") }
[void]$html.AppendLine('</div><p>Single synthetic workload per resolution. No hardware decode, D3D comparison, network, audio sync, visible desktop interaction, pure GPU timing or clean-machine validation. Smoke output is only harness validation.</p></html>')
Write-Utf8 (Join-Path $OutputDirectory 'index.html') $html.ToString()
Write-Host "Recorded JSON, CSV and charts: $OutputDirectory"

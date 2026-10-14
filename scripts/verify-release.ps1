#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidatePattern('^3\.[0-9]+\.[0-9]+(?:-[a-z0-9]+(?:[.-][a-z0-9]+)*)?$')]
    [string]$Version = '3.0.0',
    [ValidatePattern('^[a-z0-9][a-z0-9-]{0,63}$')]
    [string]$RunId = ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)),
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-NoReparse([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            if (((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Reparse point rejected: $current"
            }
        }
        $parent = [IO.Directory]::GetParent($current)
        if ($null -eq $parent) { break }
        $current = $parent.FullName
    }
}
function Get-SafeChild([string]$Root, [string]$Relative) {
    if ([IO.Path]::IsPathRooted($Relative) -or $Relative -match '[:\x00-\x1f]') { throw "Invalid relative path: $Relative" }
    foreach ($part in ($Relative -split '[/\\]')) {
        if (!$part -or $part -in @('.', '..') -or $part -match '[ .]$|[<>"|?*]' -or
            $part -match '^(?i:CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.|$)') { throw "Unsafe path component: $Relative" }
    }
    $base = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $path = [IO.Path]::GetFullPath((Join-Path $base $Relative))
    if (!$path.StartsWith($base + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw "Path escaped root: $Relative" }
    Assert-NoReparse $path
    return $path
}
function Write-Json([string]$Path, $Value) {
    Assert-NoReparse $Path
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes(($Value | ConvertTo-Json -Depth 14))
        $stream.Write($bytes, 0, $bytes.Length)
    } finally { $stream.Dispose() }
}
function Get-PeImports([string]$Path) {
    Assert-NoReparse $Path
    $bytes = [IO.File]::ReadAllBytes($Path)
    function Check-Bytes([long]$Offset, [long]$Count) {
        if ($Offset -lt 0 -or $Count -lt 0 -or $Offset -gt $bytes.LongLength - $Count) { throw 'Truncated or invalid PE structure.' }
    }
    function U16([long]$Offset) { Check-Bytes $Offset 2; return [BitConverter]::ToUInt16($bytes, [int]$Offset) }
    function U32([long]$Offset) { Check-Bytes $Offset 4; return [BitConverter]::ToUInt32($bytes, [int]$Offset) }
    function U64([long]$Offset) { Check-Bytes $Offset 8; return [BitConverter]::ToUInt64($bytes, [int]$Offset) }
    if ($bytes.LongLength -gt 512MB -or (U16 0) -ne 0x5A4D) { throw 'Not a supported DOS/PE executable.' }
    $pe = [long](U32 0x3C)
    if ((U32 $pe) -ne 0x4550 -or (U16 ($pe + 4)) -ne 0x8664) { throw 'Executable must be PE AMD64 (x64).' }
    $sectionCount = U16 ($pe + 6)
    $optionalSize = U16 ($pe + 20)
    $optional = $pe + 24
    Check-Bytes $optional $optionalSize
    if ($sectionCount -lt 1 -or $sectionCount -gt 96 -or $optionalSize -lt 240 -or (U16 $optional) -ne 0x20B -or
        (U32 ($optional + 108)) -lt 14) { throw 'Invalid PE32+ headers.' }
    $headerSize = U32 ($optional + 60)
    $imageBase = U64 ($optional + 24)
    $sections = @()
    for ($i = 0; $i -lt $sectionCount; ++$i) {
        $offset = $optional + $optionalSize + $i * 40
        Check-Bytes $offset 40
        $sections += @{ rva = [long](U32 ($offset + 12)); size = [long](U32 ($offset + 16)); raw = [long](U32 ($offset + 20)) }
    }
    function Rva-Offset([long]$Rva, [long]$Count) {
        if ($Rva -le 0 -or $Rva -gt [uint32]::MaxValue) { throw 'Invalid PE RVA.' }
        if ($Rva -lt $headerSize -and $Rva + $Count -le $headerSize) { Check-Bytes $Rva $Count; return $Rva }
        $matches = @($sections | Where-Object { $Rva -ge $_.rva -and $Rva - $_.rva + $Count -le $_.size })
        if ($matches.Count -ne 1) { throw 'PE RVA is not uniquely file-backed.' }
        $offset = $matches[0].raw + $Rva - $matches[0].rva
        Check-Bytes $offset $Count
        return $offset
    }
    function Import-Name([long]$Rva) {
        $name = New-Object Text.StringBuilder
        for ($i = 0; $i -lt 256; ++$i) {
            $value = $bytes[(Rva-Offset ($Rva + $i) 1)]
            if ($value -eq 0) {
                $text = $name.ToString().ToLowerInvariant()
                if ($text -notmatch '^[a-z0-9_-]+\.dll$') { throw 'Invalid PE import DLL name.' }
                return $text
            }
            if ($value -lt 32 -or $value -gt 126) { throw 'Invalid PE import string.' }
            $null = $name.Append([char]$value)
        }
        throw 'Unterminated PE import name.'
    }
    $ordinary = @(); $delay = @()
    foreach ($directory in @(@{ index = 1; stride = 20; delay = $false }, @{ index = 13; stride = 32; delay = $true })) {
        $rva = [long](U32 ($optional + 112 + 8 * $directory.index))
        $size = [long](U32 ($optional + 116 + 8 * $directory.index))
        if ($rva -eq 0 -and $size -eq 0) { continue }
        if ($rva -eq 0 -or $size -lt $directory.stride -or $size -gt 65536) { throw 'Invalid PE import directory.' }
        $terminated = $false
        for ($i = 0; $i -lt 256 -and ($i + 1) * $directory.stride -le $size; ++$i) {
            $offset = Rva-Offset ($rva + $i * $directory.stride) $directory.stride
            $words = @(0..($directory.stride / 4 - 1) | ForEach-Object { U32 ($offset + $_ * 4) })
            if (@($words | Where-Object { $_ -ne 0 }).Count -eq 0) { $terminated = $true; break }
            if ($directory.delay) {
                if ($words[0] -notin @(0, 1)) { throw 'Invalid delay import descriptor attributes.' }
                $nameRva = [long]$words[1]
                if ($words[0] -eq 0) {
                    if ([decimal]$nameRva -le [decimal]$imageBase) { throw 'Invalid delay-import VA.' }
                    $nameRva = [long]([decimal]$nameRva - [decimal]$imageBase)
                }
                $delay += Import-Name $nameRva
            } else { $ordinary += Import-Name ([long]$words[3]) }
        }
        if (!$terminated) { throw 'PE import directory lacks a bounded terminator.' }
    }
    if (!$ordinary.Count) { throw 'Executable has no ordinary import descriptors.' }
    return [pscustomobject]@{ machine = 'AMD64'; format = 'PE32+'; ordinary = @($ordinary | Sort-Object -Unique); delay = @($delay | Sort-Object -Unique) }
}
function Assert-SystemImports([string]$Path) {
    $imports = Get-PeImports $Path
    $allowed = @('advapi32.dll', 'bcrypt.dll', 'comctl32.dll', 'comdlg32.dll', 'd3d11.dll', 'd3dcompiler_47.dll',
        'dwmapi.dll', 'dxgi.dll', 'gdi32.dll', 'imm32.dll', 'kernel32.dll', 'mf.dll', 'mfplat.dll', 'msimg32.dll',
        'ntdll.dll', 'ole32.dll', 'oleaut32.dll', 'opengl32.dll', 'secur32.dll', 'shell32.dll', 'shlwapi.dll',
        'user32.dll', 'version.dll', 'windowscodecs.dll', 'winhttp.dll', 'winmm.dll', 'ws2_32.dll')
    foreach ($name in @($imports.ordinary) + @($imports.delay)) {
        if ($name -cnotin $allowed) { throw "Undeclared/non-system PE dependency: $name in $([IO.Path]::GetFileName($Path))" }
    }
    return $imports
}
function Get-PerformanceNames {
    $names = @('environment.json', 'results.json', 'results.csv', 'summary.json', 'summary.csv', 'index.html',
        'cpu.svg', 'cores.svg', 'memory.svg', 'private.svg', 'first-frame.svg', 'fps.svg', 'sync.svg', 'render.svg')
    foreach ($resolution in @('720p', '1080p', '4k')) {
        foreach ($mode in @('native-yuv', 'bgra')) {
            foreach ($repeat in 1..3) { $names += "raw/$resolution-$mode-$repeat.json" }
        }
    }
    return $names
}
function Assert-PerformanceEvidence([string]$Root, [switch]$CheckSourceFiles) {
    function Read-Evidence([string]$Name) {
        $path = Get-SafeChild $Root $Name
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0 -or
            (Get-Item -LiteralPath $path).Length -gt 64MB) { throw "Missing/oversized performance evidence: $Name" }
        return [IO.File]::ReadAllText($path)
    }
    function Number($Value) {
        if ($null -eq $Value -or $Value -is [bool]) { throw 'Invalid numeric performance field.' }
        $number = 0.0
        if (![double]::TryParse([string]$Value, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$number) -or
            [double]::IsNaN($number) -or [double]::IsInfinity($number)) { throw 'Nonfinite performance field.' }
        return $number
    }
    function Near($Actual, $Expected, [string]$Label, [double]$Tolerance = 0.0000001) {
        $a = Number $Actual; $b = Number $Expected
        if ([Math]::Abs($a - $b) -gt $Tolerance * [Math]::Max(1, [Math]::Abs($b))) { throw "Inconsistent performance value: $Label" }
    }
    function Equal-Value($Actual, $Expected, [string]$Label) {
        if ($Expected -is [DateTime] -or $Expected -is [DateTimeOffset]) {
            $actualTime = if ($Actual -is [DateTime]) { [DateTimeOffset]$Actual } else { [DateTimeOffset]::Parse([string]$Actual) }
            if ($actualTime -ne [DateTimeOffset]$Expected) { throw "Inconsistent performance timestamp: $Label" }
        } elseif ($Expected -is [pscustomobject]) {
            if ($Actual -isnot [pscustomobject]) { throw "Missing performance object: $Label" }
            foreach ($property in $Expected.PSObject.Properties) { Equal-Value $Actual.($property.Name) $property.Value "$Label/$($property.Name)" }
        } elseif ($Expected -is [Array]) {
            if (@($Actual).Count -ne $Expected.Count) { throw "Inconsistent performance array: $Label" }
            for ($i = 0; $i -lt $Expected.Count; ++$i) { Equal-Value $Actual[$i] $Expected[$i] "$Label/$i" }
        } elseif ($Expected -is [bool]) {
            if ([string]$Actual -ine [string]$Expected) { throw "Inconsistent performance bool: $Label" }
        } elseif ($Expected -is [ValueType]) { Near $Actual $Expected $Label }
        elseif ([string]$Actual -cne [string]$Expected) { throw "Inconsistent performance identity: $Label" }
    }
    function Label-Path([string]$Path) {
        if ($Path.StartsWith('workspace/', [StringComparison]::Ordinal)) {
            $null = Get-SafeChild $workspace $Path.Substring(10)
            return $Path
        }
        $full = [IO.Path]::GetFullPath($Path)
        if (!$full.StartsWith($workspace + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Performance identity path is outside workspace.' }
        $relative = $full.Substring($workspace.Length + 1).Replace('\', '/')
        $null = Get-SafeChild $workspace $relative
        return "workspace/$relative"
    }
    function Median($Values) { $sorted = @($Values | Sort-Object); if ($sorted.Count -ne 3) { throw 'Package requires exactly three repeats.' }; return [double]$sorted[1] }
    function Percentile($Values, [double]$P) { $sorted = @($Values | Sort-Object); return $sorted[[int][Math]::Ceiling($P * $sorted.Count) - 1] }
    function Timestamp($Value) {
        if ($Value -is [DateTime] -or $Value -is [DateTimeOffset]) { return [DateTimeOffset]$Value }
        return [DateTimeOffset]::Parse([string]$Value, [Globalization.CultureInfo]::InvariantCulture)
    }
    foreach ($name in Get-PerformanceNames) { $null = Read-Evidence $name }
    $envRecord = (Read-Evidence 'environment.json') | ConvertFrom-Json
    if ($envRecord.completed -isnot [bool] -or !$envRecord.completed -or $envRecord.purpose -cne 'serial performance measurement' -or
        $envRecord.repeats -ne 3 -or @($envRecord.fixtures).Count -ne 3 -or $envRecord.schema_version -ne 1) {
        throw 'Package performance evidence must be complete formal measurement with exactly three repeats.'
    }
    $envStart = Timestamp $envRecord.started_utc; $envEnd = Timestamp $envRecord.ended_utc
    if ($envEnd -le $envStart -or $envRecord.benchmark_sha256 -notmatch '^[a-fA-F0-9]{64}$' -or
        $envRecord.generator_sha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'Invalid measurement interval/binary identity.' }
    $null = Label-Path $envRecord.output_directory
    $null = Label-Path $envRecord.benchmark_executable
    if ($CheckSourceFiles) {
        $exe = $envRecord.benchmark_executable
        if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ine $envRecord.benchmark_sha256 -or
            (Get-FileHash -LiteralPath (Join-Path (Split-Path -Parent $exe) 'make-benchmark-media.exe') -Algorithm SHA256).Hash -ine $envRecord.generator_sha256) { throw 'Measured binary identity differs from available executable.' }
    }
    $runs = @(((Read-Evidence 'results.json') | ConvertFrom-Json).runs)
    $csv = @(Read-Evidence 'results.csv' | ConvertFrom-Csv)
    $summaries = @(((Read-Evidence 'summary.json') | ConvertFrom-Json) | ForEach-Object { $_ })
    $summaryCsv = @(Read-Evidence 'summary.csv' | ConvertFrom-Csv)
    if ($runs.Count -ne 18 -or $csv.Count -ne 18 -or $summaries.Count -ne 3 -or $summaryCsv.Count -ne 3) { throw 'Performance aggregate counts must be 18 runs and three summaries.' }
    $seen = @{}; $lastEnd = $envStart
    for ($i = 0; $i -lt 18; ++$i) {
        $run = $runs[$i]; $key = "$($run.resolution)-$($run.mode)-$($run.repeat)"
        if ($seen.ContainsKey($key) -or $run.resolution -cnotin @('720p', '1080p', '4k') -or
            $run.mode -cnotin @('native-yuv', 'bgra') -or $run.repeat -notin @(1, 2, 3) -or $run.run_order -ne $i + 1) { throw 'Unexpected/duplicate run identity.' }
        $seen[$key] = $true
        $raw = (Read-Evidence "raw/$key.json") | ConvertFrom-Json
        Equal-Value $run $raw $key
        foreach ($property in $run.PSObject.Properties) {
            if ($property.Name -notin @('frames', 'memory_samples')) { Equal-Value $csv[$i].($property.Name) $property.Value "$key/CSV/$($property.Name)" }
        }
        $fixture = @($envRecord.fixtures | Where-Object { $_.resolution -ceq $run.resolution })
        if ($fixture.Count -ne 1 -or $fixture[0].sha256 -notmatch '^[a-fA-F0-9]{64}$' -or
            (Label-Path $run.source) -cne (Label-Path $fixture[0].path) -or $run.source_bytes -ne $fixture[0].bytes -or
            (Label-Path $run.raw_json) -cne ((Label-Path $envRecord.output_directory) + "/raw/$key.json")) { throw 'Run and fixture identities differ.' }
        if ($CheckSourceFiles -and (Get-FileHash -LiteralPath $fixture[0].path -Algorithm SHA256).Hash -ine $fixture[0].sha256) { throw 'Measured source fixture changed.' }
        $dimensions = @{ '720p' = @(1280, 720); '1080p' = @(1920, 1080); '4k' = @(3840, 2160) }[$run.resolution]
        $pixel = if ($run.mode -ceq 'native-yuv') { 'yuv420p' } else { 'bgra' }
        if ($run.status -cne 'passed' -or $run.error -cne '' -or $run.renderer -cne 'OpenGLRenderer' -or $run.decoder -cne 'ffmpeg' -or
            $run.observed_pixel_format -cne $pixel -or $run.source_width -ne $dimensions[0] -or $run.source_height -ne $dimensions[1] -or
            $run.source_audio_streams -ne 0 -or $run.source_codec -cne 'mpeg4' -or $run.source_pixel_format -cne 'yuv420p' -or $run.source_container -cne 'avi' -or
            $run.window_width -ne 1280 -or $run.window_height -ne 720 -or $run.hidden_window -eq $envRecord.visible_window) { throw 'Run source/render/frame conditions differ from measurement contract.' }
        Near $run.source_fps $envRecord.requested_fps "$key/fps"
        Near $run.source_duration_seconds $envRecord.requested_seconds "$key/duration" 0.001
        Near $run.source_frames ($run.source_fps * $run.source_duration_seconds) "$key/source-frame-count" 0.001
        foreach ($field in @('elapsed_seconds', 'source_bytes', 'source_frames', 'process_cpu_seconds', 'logical_processors', 'first_frame_ms', 'peak_rss_bytes', 'peak_private_commit_bytes')) {
            if ((Number $run.$field) -le 0) { throw "Nonpositive performance field: $key/$field" }
        }
        foreach ($field in @('session_published_frames', 'session_dropped_frames', 'observer_missed_frames', 'unaccounted_source_frames')) {
            $n = Number $run.$field; if ($n -lt 0 -or $n -ne [Math]::Floor($n)) { throw 'Invalid frame counter.' }
        }
        $start = Timestamp $run.started_utc; $end = Timestamp $run.ended_utc
        if ($start -lt $lastEnd -or $end -le $start -or $end -gt $envEnd) { throw 'Performance runs overlap or escape the recorded interval.' }
        Near ($end - $start).TotalSeconds $run.elapsed_seconds "$key/elapsed-wallclock" 0.01
        $lastEnd = $end
        $frames = @($run.frames)
        if ($frames.Count -lt 2 -or $frames.Count -ne $run.unique_delivered_frames -or @($run.memory_samples).Count -lt 2) { throw 'Missing real frame/memory sequence.' }
        $serial = 0L; $elapsed = -1.0; $missed = 0L
        foreach ($frame in $frames) {
            if ($frame.serial -le $serial -or $frame.serial -ne [Math]::Floor((Number $frame.serial)) -or
                (Number $frame.elapsed_seconds) -le $elapsed -or $frame.elapsed_seconds -gt $run.elapsed_seconds -or
                (Number $frame.present_complete_ms) -lt 0 -or (Number $frame.render_complete_ms) -lt $frame.present_complete_ms) { throw 'Invalid frame sequence.' }
            $missed += $frame.serial - $serial - 1; $serial = $frame.serial; $elapsed = $frame.elapsed_seconds
        }
        Near $run.session_published_frames $serial "$key/published"
        Near $run.observer_missed_frames $missed "$key/missed"
        Near $run.source_frames ($run.session_published_frames + $run.session_dropped_frames + $run.unaccounted_source_frames) "$key/conservation"
        Near $run.first_frame_ms ($frames[0].elapsed_seconds * 1000) "$key/first-frame"
        Near $run.delivered_fps ($frames.Count / $run.elapsed_seconds) "$key/delivered-fps"
        Near $run.presentation_span_fps (($frames.Count - 1) / ($frames[-1].elapsed_seconds - $frames[0].elapsed_seconds)) "$key/presentation-fps"
        Near $run.cpu_core_equivalents ($run.process_cpu_seconds / $run.elapsed_seconds) "$key/cpu-core"
        Near $run.cpu_one_core_percent ($run.cpu_core_equivalents * 100) "$key/cpu-one"
        Near $run.cpu_machine_percent ($run.cpu_one_core_percent / $run.logical_processors) "$key/cpu-machine"
        Near $run.cpu_seconds_per_delivered_frame ($run.process_cpu_seconds / $frames.Count) "$key/cpu-frame"
        Near $run.peak_rss_mib ($run.peak_rss_bytes / 1MB) "$key/rss"
        Near $run.peak_private_commit_mib ($run.peak_private_commit_bytes / 1MB) "$key/commit"
        foreach ($p in @(@{ label = 'p50'; value = 0.5 }, @{ label = 'p95'; value = 0.95 })) {
            foreach ($metric in @(@{ name = 'sync_signed'; values = @($frames.sync_difference_ms) },
                @{ name = 'sync_abs'; values = @($frames | ForEach-Object { [Math]::Abs($_.sync_difference_ms) }) },
                @{ name = 'render_complete'; values = @($frames.render_complete_ms) }, @{ name = 'present_complete'; values = @($frames.present_complete_ms) })) {
                Near $run.($metric.name + '_' + $p.label + '_ms') (Percentile $metric.values $p.value) "$key/$($metric.name)/$($p.label)"
            }
        }
        foreach ($sample in $run.memory_samples) {
            if ((Number $sample.elapsed_seconds) -lt 0 -or $sample.elapsed_seconds -gt $run.elapsed_seconds -or
                (Number $sample.rss_bytes) -le 0 -or $sample.rss_bytes -gt $run.peak_rss_bytes -or
                (Number $sample.private_bytes) -le 0 -or $sample.private_bytes -gt $run.peak_sampled_private_bytes) { throw 'Memory sample exceeds declared bounds.' }
        }
    }
    foreach ($resolution in @('720p', '1080p', '4k')) {
        $group = @($runs | Where-Object { $_.resolution -ceq $resolution })
        $yuv = @($group | Where-Object { $_.mode -ceq 'native-yuv' }); $bgra = @($group | Where-Object { $_.mode -ceq 'bgra' })
        $summary = @($summaries | Where-Object { $_.resolution -ceq $resolution }); $row = @($summaryCsv | Where-Object { $_.resolution -ceq $resolution })
        if ($summary.Count -ne 1 -or $row.Count -ne 1 -or $yuv.Count -ne 3 -or $bgra.Count -ne 3) { throw 'Incomplete resolution comparison.' }
        foreach ($property in $summary[0].PSObject.Properties) {
            if ($property.Name -eq 'comparison_gate') { continue } # Export-Csv stores this nested object as a display string.
            Equal-Value $row[0].($property.Name) $property.Value "$resolution/summary-csv/$($property.Name)"
        }
        Near $summary[0].native_repeats 3 "$resolution/native-count"; Near $summary[0].bgra_repeats 3 "$resolution/bgra-count"
        foreach ($mode in @(@{ prefix = 'native'; rows = $yuv }, @{ prefix = 'bgra'; rows = $bgra })) {
            foreach ($metric in @(@{ target = 'cpu_median_seconds'; source = 'process_cpu_seconds' }, @{ target = 'rss_median_mib'; source = 'peak_rss_mib' },
                @{ target = 'delivered_frames_median'; source = 'unique_delivered_frames' })) {
                Near $summary[0].($mode.prefix + '_' + $metric.target) (Median @($mode.rows | ForEach-Object { $_.($metric.source) })) "$resolution/summary/$($metric.target)"
            }
            foreach ($metric in @(@{ target = 'drop_total'; source = 'session_dropped_frames' }, @{ target = 'observer_missed_total'; source = 'observer_missed_frames' })) {
                Near $summary[0].($mode.prefix + '_' + $metric.target) (($mode.rows | Measure-Object -Property $metric.source -Sum).Sum) "$resolution/summary/$($metric.target)"
            }
        }
        $comparable = $true
        foreach ($field in @('source_sha256', 'benchmark_sha256', 'source_width', 'source_height', 'source_fps', 'source_frames', 'source_duration_seconds',
            'source_bytes', 'source_codec', 'source_pixel_format', 'source_container', 'source_audio_streams', 'renderer', 'decoder',
            'gl_vendor', 'gl_renderer', 'gl_version', 'window_width', 'window_height', 'hidden_window', 'swap_interval', 'swap_interval_disable_succeeded')) {
            $values = @($group | ForEach-Object { $_.$field })
            if ($values.Count -ne 6 -or @($values | Where-Object { $null -eq $_ -or [string]$_ -eq '' }).Count -or @($values | Select-Object -Unique).Count -ne 1) {
                $comparable = $false
            }
        }
        foreach ($run in $group) {
            if ($run.source_sha256 -ine @($envRecord.fixtures | Where-Object { $_.resolution -ceq $resolution })[0].sha256 -or
                $run.benchmark_sha256 -ine $envRecord.benchmark_sha256 -or $run.source_hash_changed_during_run -isnot [bool]) { throw 'Missing or conflicting aggregate hash evidence.' }
            if ($run.source_hash_changed_during_run -or $run.unique_delivered_frames -ne $run.source_frames -or $run.session_published_frames -ne $run.source_frames -or
                $run.session_dropped_frames -ne 0 -or $run.observer_missed_frames -ne 0 -or $run.unaccounted_source_frames -ne 0 -or
                @($run.frames).Count -ne $run.source_frames -or $run.swap_interval -eq -999) { $comparable = $false }
        }
        $gate = $summary[0].comparison_gate
        if ($gate.comparable -isnot [bool] -or $gate.comparable -ne $comparable -or $gate.required_session_drops -ne 0 -or
            $gate.required_observer_misses -ne 0 -or $gate.required_unaccounted_frames -ne 0) { throw 'Summary comparison gate conflicts with actual identities/frame conditions.' }
        if ($comparable) {
            if ($summary[0].comparison_status -cne 'comparable observations' -or $summary[0].comparison_reasons -cne '' -or @($gate.reasons).Count -ne 0) { throw 'False comparison summary.' }
            $a = Median $yuv.process_cpu_seconds; $b = Median $bgra.process_cpu_seconds
            Near $summary[0].native_cpu_reduction_percent (100 * ($b - $a) / $b) "$resolution/cpu-reduction"
        } elseif ($null -ne $summary[0].native_cpu_reduction_percent -or $summary[0].comparison_status -cne 'inconclusive' -or
            !$summary[0].comparison_reasons -or @($gate.reasons).Count -eq 0) { throw 'Inconclusive observations must not assert a reduction.' }
    }
    # Validate plotted bars and dots against the same run values, including
    # geometry. Never regenerate or substitute a chart while accepting evidence.
    $charts = @(@('cpu.svg', 'process_cpu_seconds'), @('cores.svg', 'cpu_core_equivalents'), @('memory.svg', 'peak_rss_mib'),
        @('private.svg', 'peak_private_commit_mib'), @('first-frame.svg', 'first_frame_ms'), @('fps.svg', 'delivered_fps'),
        @('sync.svg', 'sync_abs_p95_ms'), @('render.svg', 'render_complete_p95_ms'))
    foreach ($chart in $charts) {
        $settings = New-Object Xml.XmlReaderSettings; $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit; $settings.XmlResolver = $null
        $reader = [Xml.XmlReader]::Create((New-Object IO.StringReader((Read-Evidence $chart[0]))), $settings)
        $xml = New-Object Xml.XmlDocument; $xml.XmlResolver = $null
        try { $xml.Load($reader) } finally { $reader.Dispose() }
        if ($xml.DocumentElement.LocalName -cne 'svg') { throw 'Invalid SVG root.' }
        $bars = @($xml.SelectNodes("//*[local-name()='rect' and @width='50']")); $dots = @($xml.SelectNodes("//*[local-name()='circle']"))
        if ($bars.Count -ne 6 -or $dots.Count -ne 18) { throw 'Incomplete performance chart.' }
        $max = ($runs | Measure-Object -Property $chart[1] -Maximum).Maximum; if ($max -le 0) { $max = 1 }; $scale = 275 / $max
        $barIndex = 0; $dotIndex = 0; $groupIndex = 0
        foreach ($resolution in @('720p', '1080p', '4k')) {
            $cx = 75 + 260 * ($groupIndex + 0.5); $modeIndex = 0
            foreach ($mode in @('native-yuv', 'bgra')) {
                $points = @($runs | Where-Object { $_.resolution -ceq $resolution -and $_.mode -ceq $mode } | ForEach-Object { $_.($chart[1]) })
                $median = Median $points; $x = $cx - 58 + $modeIndex * 65
                Near $bars[$barIndex].GetAttribute('x') $x "$($chart[0])/bar-x" 0.001
                Near $bars[$barIndex].GetAttribute('y') (335 - $median * $scale) "$($chart[0])/bar-y" 0.001
                Near $bars[$barIndex].GetAttribute('height') ($median * $scale) "$($chart[0])/bar-height" 0.001
                for ($p = 0; $p -lt 3; ++$p) {
                    Near $dots[$dotIndex].GetAttribute('cx') ($x + 10 + $p * 9) "$($chart[0])/dot-x" 0.001
                    Near $dots[$dotIndex].GetAttribute('cy') (335 - $points[$p] * $scale) "$($chart[0])/dot-y" 0.001
                    $title = $dots[$dotIndex].SelectSingleNode("*[local-name()='title']").InnerText
                    $formatted = ([double]$points[$p]).ToString('0.###', [Globalization.CultureInfo]::InvariantCulture)
                    if (!$title.StartsWith("$mode run $($p + 1): $formatted ", [StringComparison]::Ordinal)) { throw 'Chart label does not match raw observation.' }
                    ++$dotIndex
                }
                ++$barIndex; ++$modeIndex
            }
            ++$groupIndex
        }
    }
    $html = Read-Evidence 'index.html'
    foreach ($name in @('results.json', 'results.csv', 'environment.json', 'summary.csv')) {
        if (!$html.Contains("href=`"$name`"")) { throw 'Performance HTML lacks evidence link.' }
    }
    foreach ($chart in $charts) { if (!$html.Contains("src='$($chart[0])'")) { throw 'Performance HTML lacks chart link.' } }
    $tableRows = @([regex]::Matches($html, '<tr><td>(.*?)</td>(.*?)</tr>'))
    if ($tableRows.Count -ne 3) { throw 'Incomplete performance HTML summary.' }
    foreach ($summary in $summaries) {
        $row = @($tableRows | Where-Object { $_.Groups[1].Value -ceq $summary.resolution })
        if ($row.Count -ne 1) { throw 'HTML resolution summary differs.' }
        $cells = @([regex]::Matches($row[0].Groups[2].Value, '<td>(.*?)</td>') | ForEach-Object { $_.Groups[1].Value })
        $expected = @($summary.native_cpu_median_seconds, $summary.bgra_cpu_median_seconds)
        for ($i = 0; $i -lt 2; ++$i) {
            $label = ([double]$expected[$i]).ToString('0.###', [Globalization.CultureInfo]::InvariantCulture)
            if ($cells[$i] -cne $label) { throw 'HTML numeric summary differs.' }
        }
        $comparison = if ($summary.comparison_gate.comparable) { ([double]$summary.native_cpu_reduction_percent).ToString('0.###', [Globalization.CultureInfo]::InvariantCulture) + '%' }
            else { 'inconclusive: ' + [Net.WebUtility]::HtmlEncode($summary.comparison_reasons) }
        if ($cells[2] -cne $comparison) { throw 'HTML comparison assertion differs from actual gate.' }
    }
    return [pscustomobject]@{ repeats = 3; runs = 18; rawFiles = 18; charts = 8; consistent = $true }
}
function Get-StabilityNames {
    return @('result.json', 'trajectory-verification.json', 'samples.csv', 'process-exit.json')
}
function Assert-StabilityEvidence([string]$Root) {
    foreach ($name in Get-StabilityNames) {
        $path = Get-SafeChild $Root $name
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0 -or
            (Get-Item -LiteralPath $path).Length -gt 64MB) { throw "Missing/oversized stability evidence: $name" }
    }
    $result = [IO.File]::ReadAllText((Get-SafeChild $Root 'result.json')) | ConvertFrom-Json
    $trajectory = [IO.File]::ReadAllText((Get-SafeChild $Root 'trajectory-verification.json')) | ConvertFrom-Json
    $processExit = [IO.File]::ReadAllText((Get-SafeChild $Root 'process-exit.json')) | ConvertFrom-Json
    $duration = 0.0
    if ($result.passed -isnot [bool] -or !$result.passed -or
        ![double]::TryParse([string]$result.elapsedSeconds, [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$duration) -or
        [double]::IsNaN($duration) -or [double]::IsInfinity($duration) -or $duration -lt 3600 -or
        $result.completedCycles -lt 100 -or $result.operations -lt 500 -or
        $trajectory.passed -isnot [bool] -or !$trajectory.passed) {
        throw 'Stability evidence requires passed=true, elapsedSeconds>=3600, >=100 cycles, >=500 operations and trajectory passed=true.'
    }
    if ($trajectory.resultSHA256 -cne (Get-FileHash -LiteralPath (Get-SafeChild $Root 'result.json') -Algorithm SHA256).Hash -or
        $trajectory.samplesSHA256 -cne (Get-FileHash -LiteralPath (Get-SafeChild $Root 'samples.csv') -Algorithm SHA256).Hash) {
        throw 'Stability trajectory hashes differ from the included result or samples.'
    }
    if ($null -ne $processExit.exitCode) {
        if ($processExit.exitCode -is [bool] -or [string]$processExit.exitCode -cne '0') { throw 'Observed stability exit code is nonzero or invalid.' }
    } else {
        # Retain an unavailable observation as a measurement limitation, never as exitCode=0.
        $unavailable = $processExit.PSObject.Properties['exitCodeUnavailable']
        $reason = $processExit.PSObject.Properties['exitCodeUnavailableReason']
        if ($null -eq $unavailable -or $unavailable.Value -isnot [bool] -or !$unavailable.Value -or
            $null -eq $reason -or $reason.Value -isnot [string] -or [string]::IsNullOrWhiteSpace($reason.Value)) {
            throw 'Unavailable stability exit code requires an explicit limitation and reason.'
        }
    }
    $observedExit = [DateTimeOffset]::MinValue
    $finished = [DateTimeOffset]::MinValue
    if (![DateTimeOffset]::TryParse([string]$processExit.observedExitUtc, [ref]$observedExit) -or
        ![DateTimeOffset]::TryParse([string]$result.finishedUtc, [ref]$finished) -or
        $observedExit -lt $finished -or $processExit.binarySHA256 -cnotmatch '^[A-Fa-f0-9]{64}$') {
        throw 'Stability process termination must be observed after completion with a binary identity.'
    }
}
function Read-Manifest([string]$Root) {
    $path = Get-SafeChild $Root 'manifest.json'
    $manifest = [IO.File]::ReadAllText($path) | ConvertFrom-Json
    if ($manifest.schemaVersion -ne 3 -or $manifest.version -cne $Version -or
        $manifest.packageName -cne $packageName -or $manifest.platform -cne 'windows-x64' -or
        $manifest.configuration -cne 'Release' -or $manifest.hashAlgorithm -cne 'SHA256') { throw 'Invalid manifest contract.' }
    $expected = @{}
    foreach ($entry in $manifest.files) {
        $null = Get-SafeChild $Root $entry.path
        if ($entry.path.Contains('\') -or $expected.ContainsKey($entry.path) -or $entry.path -eq 'manifest.json' -or
            $entry.sha256 -cnotmatch '^[a-f0-9]{64}$' -or $entry.bytes -le 0 -or
            $entry.role -notin @('binary', 'resource', 'documentation', 'license', 'launcher', 'measurement')) { throw "Invalid/duplicate manifest entry: $($entry.path)" }
        $expected[$entry.path] = $entry
    }
    $required = @('native-player.exe', 'native-service.exe', 'native-demo-http.exe', 'launch.cmd', 'launch-demo.cmd', 'media/stream.flv', 'media/hls/index.m3u8',
        'licenses/imgui-LICENSE.txt', 'licenses/ffmpeg-copyright.txt', 'licenses/nlohmann-json-copyright.txt', 'licenses/NOTICE.md',
        'docs/delivery/user-guide.md', 'docs/delivery/architecture.md', 'docs/delivery/demo.md', 'docs/delivery/acceptance.md',
        'docs/protocol/service-v1.md', 'docs/demo/second-layer.md', 'docs/validation/2026-10-13-first-layer.md', 'docs/validation/2026-10-13-second-layer.md') +
        @(0..7 | ForEach-Object { 'media/hls/segment-{0:D2}.ts' -f $_ })
    $optional = @('docs/validation/2026-10-13-third-layer.md', 'docs/performance/method.md')
    $stabilityNames = @(Get-StabilityNames | ForEach-Object { "docs/validation/stability/$_" })
    $optional += $stabilityNames
    foreach ($name in @('environment.json', 'results.json', 'results.csv', 'summary.json', 'summary.csv', 'index.html',
        'cpu.svg', 'cores.svg', 'memory.svg', 'private.svg', 'first-frame.svg', 'fps.svg', 'sync.svg', 'render.svg')) {
        $optional += "docs/performance/results/$name"
    }
    foreach ($resolution in @('720p', '1080p', '4k')) {
        foreach ($mode in @('native-yuv', 'bgra')) {
            foreach ($repeat in 1..3) { $optional += "docs/performance/results/raw/$resolution-$mode-$repeat.json" }
        }
    }
    foreach ($name in $required) { if (!$expected.ContainsKey($name)) { throw "Required package file missing from manifest: $name" } }
    foreach ($name in $expected.Keys) { if ($name -cnotin $required -and $name -cnotin $optional) { throw "Package file outside delivery whitelist: $name" } }
    $performanceNames = @(Get-PerformanceNames | ForEach-Object { "docs/performance/results/$_" })
    $performanceCount = @($performanceNames | Where-Object { $expected.ContainsKey($_) }).Count
    if ($performanceCount -ne 0 -and $performanceCount -ne $performanceNames.Count) { throw 'Partial performance evidence in manifest.' }
    $stabilityCount = @($stabilityNames | Where-Object { $expected.ContainsKey($_) }).Count
    if ($stabilityCount -ne 0 -and $stabilityCount -ne 4) { throw 'Partial stability evidence in manifest.' }
    if ($stabilityCount -eq 4) { Assert-StabilityEvidence (Get-SafeChild $Root 'docs/validation/stability') }
    foreach ($exe in @('native-player.exe', 'native-service.exe', 'native-demo-http.exe')) {
        $actual = Assert-SystemImports (Get-SafeChild $Root $exe)
        if (($actual | ConvertTo-Json -Compress -Depth 4) -cne ($manifest.peImports.$exe | ConvertTo-Json -Compress -Depth 4)) { throw "Manifest PE dependency evidence differs: $exe" }
    }
    if ($performanceCount -gt 0) { $null = Assert-PerformanceEvidence (Get-SafeChild $Root 'docs/performance/results') }
    return $manifest
}
function Assert-Package([string]$Root) {
    $manifest = Read-Manifest $Root
    $expected = @{}
    foreach ($entry in $manifest.files) { $expected[$entry.path] = $entry }
    $allowedDirectories = @{}
    foreach ($entry in $manifest.files) {
        $parts = $entry.path -split '/'
        for ($i = 1; $i -lt $parts.Count; ++$i) { $allowedDirectories[($parts[0..($i - 1)] -join '/')] = $true }
    }
    $seen = @{}
    # Walk explicitly, rejecting directory links before recursing into them.
    $pending = New-Object 'Collections.Generic.Stack[string]'
    $pending.Push($Root)
    while ($pending.Count -gt 0) {
        $directory = $pending.Pop()
        Assert-NoReparse $directory
        foreach ($item in Get-ChildItem -LiteralPath $directory -Force) {
            Assert-NoReparse $item.FullName
            $relative = $item.FullName.Substring($Root.TrimEnd('\', '/').Length + 1).Replace('\', '/')
            if ($item.PSIsContainer) {
                if (!$allowedDirectories.ContainsKey($relative)) { throw "Unlisted package directory rejected: $relative" }
                $pending.Push($item.FullName); continue
            }
            if ($relative -eq 'manifest.json') { continue }
            if (!$expected.ContainsKey($relative)) { throw "Unlisted package file (including runtime data) rejected: $relative" }
            $entry = $expected[$relative]
            if ($item.Length -ne $entry.bytes -or (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256) {
                throw "SHA256/size mismatch: $relative"
            }
            $seen[$relative] = $true
        }
    }
    if ($seen.Count -ne $expected.Count) { throw 'One or more manifest files are missing.' }
    $playlist = [IO.File]::ReadAllLines((Get-SafeChild $Root 'media/hls/index.m3u8'))
    $references = @($playlist | Where-Object { $_ -and !$_.StartsWith('#') })
    if (($references -join '|') -cne (@(0..7 | ForEach-Object { 'segment-{0:D2}.ts' -f $_ }) -join '|') -or $playlist -notcontains '#EXT-X-ENDLIST') {
        throw 'Unexpected or escaping HLS references.'
    }
    return $manifest
}
function Quote-Argument([string]$Value) {
    if ($Value -match '["\r\n\x00]') { throw 'Unsupported character in process argument.' }
    # None of our arguments end in a slash; rejecting that case avoids the
    # Windows command-line backslash/closing-quote ambiguity on PowerShell 5.1.
    if ($Value.EndsWith('\')) { throw 'Trailing backslash in process argument.' }
    return '"' + $Value + '"'
}
function Connect-Bounded([int]$Port) {
    $client = New-Object Net.Sockets.TcpClient
    try {
        $connection = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
        try {
            if (!$connection.AsyncWaitHandle.WaitOne(3000)) { throw 'Loopback connect deadline exceeded.' }
            $client.EndConnect($connection)
        } finally { $connection.AsyncWaitHandle.Close() }
        $client.ReceiveTimeout = 3000; $client.SendTimeout = 3000
        return $client
    } catch { $client.Dispose(); throw }
}
function Read-Exact([IO.Stream]$Stream, [int]$Count) {
    if ($Count -lt 1 -or $Count -gt 2097152) { throw 'Invalid bounded response length.' }
    $buffer = New-Object byte[] $Count; $offset = 0
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ($offset -lt $Count) {
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Response read deadline exceeded.' }
        $read = $Stream.Read($buffer, $offset, $Count - $offset)
        if ($read -eq 0) { throw 'Peer disconnected before complete response.' }
        $offset += $read
    }
    return ,$buffer
}
function Test-ServiceProtocol([int]$Port) {
    $client = Connect-Bounded $Port
    try {
        $stream = $client.GetStream(); $responses = @()
        foreach ($version in @(1, 999)) {
            $id = 'package-' + [Guid]::NewGuid().ToString('N')
            $body = [Text.Encoding]::UTF8.GetBytes((@{ version = $version; id = $id; op = 'heartbeat'; body = @{} } | ConvertTo-Json -Compress))
            $prefix = [BitConverter]::GetBytes([uint32]$body.Length); [Array]::Reverse($prefix)
            $stream.Write($prefix, 0, 4); $stream.Write($body, 0, $body.Length)
            $lengthBytes = Read-Exact $stream 4; [Array]::Reverse($lengthBytes)
            $length = [BitConverter]::ToUInt32($lengthBytes, 0)
            if ($length -lt 1 -or $length -gt 2097152) { throw 'Invalid service frame length.' }
            $response = [Text.Encoding]::UTF8.GetString((Read-Exact $stream $length)) | ConvertFrom-Json
            $expectedCode = if ($version -eq 1) { 'OK' } else { 'BAD_VERSION' }
            if ($response.version -ne 1 -or $response.id -cne $id -or $response.op -cne 'heartbeat' -or
                $response.code -cne $expectedCode -or $response.ok -isnot [bool] -or $response.ok -ne ($version -eq 1) -or
                $response.body -isnot [pscustomobject]) { throw 'Service protocol identity/version contract failed.' }
            $responses += $response
        }
        return [pscustomobject]@{ port = $Port; frames = $responses; passed = $true }
    } finally { $client.Dispose() }
}
function Test-HttpRange([int]$Port, [string]$MediaRoot) {
    $records = @()
    foreach ($resource in @(@{ path = 'stream.flv'; first = 17; last = 272 }, @{ path = 'hls/index.m3u8'; first = 0; last = 63 },
        @{ path = 'hls/segment-00.ts'; first = 17; last = 272 })) {
        $file = Get-SafeChild $MediaRoot $resource.path
        $length = (Get-Item -LiteralPath $file).Length
        $expectedSize = $resource.last - $resource.first + 1
        $fileStream = [IO.File]::OpenRead($file)
        try { $null = $fileStream.Seek($resource.first, [IO.SeekOrigin]::Begin); $expectedBytes = Read-Exact $fileStream $expectedSize }
        finally { $fileStream.Dispose() }
        $client = Connect-Bounded $Port
        try {
            $stream = $client.GetStream()
            $request = [Text.Encoding]::ASCII.GetBytes("GET /$($resource.path) HTTP/1.1`r`nHost: 127.0.0.1:$Port`r`nRange: bytes=$($resource.first)-$($resource.last)`r`nConnection: close`r`n`r`n")
            $stream.Write($request, 0, $request.Length)
            $header = New-Object Text.StringBuilder; $deadline = [DateTime]::UtcNow.AddSeconds(5)
            while (!$header.ToString().EndsWith("`r`n`r`n")) {
                if ($header.Length -ge 16384 -or [DateTime]::UtcNow -ge $deadline) { throw 'HTTP header bound exceeded.' }
                $byte = $stream.ReadByte(); if ($byte -lt 0) { throw 'HTTP header truncated.' }
                $null = $header.Append([char]$byte)
            }
            $lines = $header.ToString() -split "`r`n"; $headers = @{}
            foreach ($line in $lines[1..($lines.Count - 1)]) {
                if (!$line) { continue }; $colon = $line.IndexOf(':')
                if ($colon -le 0) { throw 'Invalid HTTP header.' }
                $name = $line.Substring(0, $colon).ToLowerInvariant()
                if ($headers.ContainsKey($name)) { throw 'Duplicate HTTP header.' }
                $headers[$name] = $line.Substring($colon + 1).Trim()
            }
            if ($lines[0] -cne 'HTTP/1.1 206 Partial Content' -or $headers['content-length'] -ne [string]$expectedSize -or
                $headers['content-range'] -cne "bytes $($resource.first)-$($resource.last)/$length" -or
                $headers['accept-ranges'] -cne 'bytes' -or $headers['etag'] -notmatch '^"demo-[0-9]+--?[0-9]+"$') { throw 'HTTP Range status/headers failed.' }
            $actualBytes = Read-Exact $stream $expectedSize
            if ([BitConverter]::ToString($actualBytes) -cne [BitConverter]::ToString($expectedBytes)) { throw 'HTTP Range returned different media bytes.' }
            if ($stream.ReadByte() -ne -1) { throw 'HTTP response exceeded declared range body.' }
            $records += [pscustomobject]@{ resource = $resource.path; status = 206; first = $resource.first; last = $resource.last; bytes = $expectedSize; total = $length; etag = $headers['etag']; byteExact = $true }
        } finally { $client.Dispose() }
    }
    return [pscustomobject]@{ port = $Port; requests = $records; passed = $true }
}
function Invoke-OwnedProcess([string]$Executable, [string[]]$Arguments, [string]$CaseRoot, [int]$TimeoutSeconds,
    [string]$Probe = '', [string]$MediaRoot = '') {
    Assert-NoReparse $Executable
    Assert-NoReparse $CaseRoot
    [IO.Directory]::CreateDirectory($CaseRoot) | Out-Null
    foreach ($name in @('temp', 'local', 'roaming', 'profile')) { [IO.Directory]::CreateDirectory((Get-SafeChild $CaseRoot $name)) | Out-Null }
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $Executable
    $info.Arguments = ($Arguments | ForEach-Object { Quote-Argument $_ }) -join ' '
    $info.WorkingDirectory = $outsideCwd
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true # GUI windows still launch for native-player.
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.EnvironmentVariables.Clear()
    $info.EnvironmentVariables['SystemRoot'] = $windowsRoot
    $info.EnvironmentVariables['WINDIR'] = $windowsRoot
    $info.EnvironmentVariables['PATH'] = $outsideCwd
    $info.EnvironmentVariables['COMSPEC'] = (Join-Path $outsideCwd 'cmd.exe')
    $info.EnvironmentVariables['TEMP'] = (Join-Path $CaseRoot 'temp')
    $info.EnvironmentVariables['TMP'] = (Join-Path $CaseRoot 'temp')
    $info.EnvironmentVariables['LOCALAPPDATA'] = (Join-Path $CaseRoot 'local')
    $info.EnvironmentVariables['APPDATA'] = (Join-Path $CaseRoot 'roaming')
    $info.EnvironmentVariables['USERPROFILE'] = (Join-Path $CaseRoot 'profile')
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    $started = [DateTime]::UtcNow
    $record = [ordered]@{ executable = [IO.Path]::GetFileName($Executable); arguments = $Arguments; cwd = $outsideCwd; path = $outsideCwd;
        startedUtc = $started.ToString('o'); exitCode = $null; timedOut = $false; passed = $false }
    try {
        if (!$process.Start()) { throw 'Child process did not start.' }
        $record.pid = $process.Id
        $stderr = $process.StandardError.ReadToEndAsync()
        $deadline = $started.AddSeconds($TimeoutSeconds)
        $startupLine = ''
        if ($Probe) {
            $lineTask = $process.StandardOutput.ReadLineAsync()
            while (!$lineTask.Wait(100)) {
                if ($process.HasExited -or [DateTime]::UtcNow -ge $deadline) { throw 'Auxiliary startup failed or timed out.' }
            }
            $startupLine = $lineTask.GetAwaiter().GetResult()
            $pattern = if ($Probe -ceq 'service') { '^native-service listening on 127\.0\.0\.1:([0-9]+) data=' } else { '^Demo FLV: http://127\.0\.0\.1:([0-9]+)/stream\.flv$' }
            if (!$startupLine -or $startupLine -notmatch $pattern -or $process.HasExited) { throw 'Auxiliary startup did not announce its owned loopback listener.' }
            $port = [int]$Matches[1]; if ($port -lt 1 -or $port -gt 65535) { throw 'Invalid auxiliary listener port.' }
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $record.probe = if ($Probe -ceq 'service') { Test-ServiceProtocol $port } else { Test-HttpRange $port $MediaRoot }
            if ($process.HasExited -or [DateTime]::UtcNow -ge $deadline) { throw 'Auxiliary died or exceeded its probe deadline.' }
            $record.terminatedAfterProbe = $true
            $process.Kill()
            if (!$process.WaitForExit(10000)) { throw 'Owned auxiliary did not terminate after successful probe.' }
        } else { $stdout = $process.StandardOutput.ReadToEndAsync() }
        while (!$process.WaitForExit(1000)) {
            if ([DateTime]::UtcNow -ge $deadline) {
                $record.timedOut = $true
                # Only this invocation's retained process handle is terminated.
                $process.Kill()
                if (!$process.WaitForExit(10000)) { throw 'Owned child did not terminate after timeout.' }
                break
            }
        }
        $process.WaitForExit()
        $record.exitCode = $process.ExitCode
        [IO.File]::WriteAllText((Get-SafeChild $CaseRoot 'stdout.log'), ($startupLine + "`r`n" + $stdout.GetAwaiter().GetResult()), (New-Object Text.UTF8Encoding($false)))
        [IO.File]::WriteAllText((Get-SafeChild $CaseRoot 'stderr.log'), $stderr.GetAwaiter().GetResult(), (New-Object Text.UTF8Encoding($false)))
        $record.passed = (!$record.timedOut -and (($Probe -and $record.probe.passed) -or (!$Probe -and $record.exitCode -eq 0)))
    } catch { $record.error = $_.Exception.Message }
    finally {
        if ($record.Contains('pid') -and !$process.HasExited) { $process.Kill(); $null = $process.WaitForExit(10000) }
        $process.Dispose()
        $ended = [DateTime]::UtcNow
        $record.endedUtc = $ended.ToString('o')
        $record.elapsedSeconds = ($ended - $started).TotalSeconds
    }
    return [pscustomobject]$record
}
function Assert-SmokeResult([string]$Directory, [bool]$SecondLayer) {
    $jsonPath = Get-SafeChild $Directory 'result.json'
    $result = [IO.File]::ReadAllText($jsonPath) | ConvertFrom-Json
    if ($result.passed -isnot [bool] -or !$result.passed) { throw 'App did not report passed=true.' }
    if ($SecondLayer) {
        $expectedUploadBytes = (Get-Item -LiteralPath (Get-SafeChild $stage 'media/stream.flv')).Length
        $expectedCacheBytes = 0L
        foreach ($i in 0..7) { $expectedCacheBytes += (Get-Item -LiteralPath (Get-SafeChild $stage ('media/hls/segment-{0:D2}.ts' -f $i))).Length }
        if ($result.stages -ne 15 -or $result.segments -ne 8 -or $result.authenticated -ne $true -or
            $result.uploadedBytes -ne $expectedUploadBytes -or $result.cachedBytes -ne $expectedCacheBytes) { throw 'Fifteen-stage/bytes/segments/authentication contract failed.' }
        $screenshots = @('network', 'progressive-playing', 'download-paused', 'download-cancelled', 'download-resumed', 'account',
            'upload-paused', 'transfers-complete', 'reconnecting', 'session-restored', 'hls-download-first', 'hls-complete-playing')
    } else {
        $screenshots = @('playing', 'paused', 'seek-preview', 'idle', 'error')
        if (($result.stages -join '|') -cne ($screenshots -join '|')) { throw 'Five-stage desktop smoke contract failed.' }
    }
    foreach ($name in $screenshots) {
        $path = Get-SafeChild $Directory "$name.png"
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -lt 8) { throw "Screenshot missing: $name" }
        $stream = [IO.File]::OpenRead($path)
        try {
            $signature = New-Object byte[] 8
            $null = $stream.Read($signature, 0, 8)
            if ([BitConverter]::ToString($signature) -cne '89-50-4E-47-0D-0A-1A-0A') { throw "Invalid PNG: $name" }
        } finally { $stream.Dispose() }
    }
    return $result
}

$workspace = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
Assert-NoReparse $workspace
$packageName = "native-player-$Version-win-x64"
$release = Get-SafeChild $workspace 'build/release'
$stage = Get-SafeChild $release $packageName
$zip = Get-SafeChild $release "$packageName.zip"
$runRoot = Get-SafeChild $workspace "build/third-layer/package-verify/$Version-$RunId"
$claimPath = Get-SafeChild $workspace "build/third-layer/package-verify/$Version-$RunId.claim"
if ((Test-Path -LiteralPath $runRoot) -or (Test-Path -LiteralPath $claimPath)) { throw 'Verification output is occupied; use a fresh RunId.' }
$null = Assert-Package $stage
$zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$windowsRoot = [Environment]::GetEnvironmentVariable('SystemRoot')
if (!$windowsRoot) { throw 'Windows SystemRoot is unavailable.' }
Assert-NoReparse $windowsRoot
$outsideCwd = [IO.Path]::GetFullPath((Join-Path $windowsRoot 'System32'))
Assert-NoReparse $outsideCwd
if ($outsideCwd.StartsWith($workspace.TrimEnd('\', '/') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Verification cwd must be outside the source checkout.' }
[IO.Directory]::CreateDirectory((Split-Path -Parent $runRoot)) | Out-Null
Write-Json $claimPath @{ version = $Version; runId = $RunId; createdUtc = [DateTime]::UtcNow.ToString('o') }
if (Test-Path -LiteralPath $runRoot) { throw 'Verification directory appeared during reservation.' }
[IO.Directory]::CreateDirectory($runRoot) | Out-Null
$report = [ordered]@{
    schemaVersion = 3; version = $Version; prepared = $false; runtimeVerified = $false; passed = $false
    startedUtc = [DateTime]::UtcNow.ToString('o'); zipSHA256 = $zipHash
    stage = $stage; zip = $zip; output = $runRoot; cwd = $outsideCwd; childPath = $outsideCwd
    isolation = 'Same Windows machine; separate build output, cleared child environment and cwd outside source checkout.'
    cleanMachineVerified = $false; tests = @(); limits = @('No clean VM or second-machine run.', 'Windows GPU/audio/system DLLs remain those of this host.', 'Launch CMD interaction is a manual acceptance item.')
}
try {
    Add-Type -AssemblyName System.IO.Compression
    $extractRoot = Get-SafeChild $runRoot 'extracted'
    [IO.Directory]::CreateDirectory($extractRoot) | Out-Null
    $zipStream = [IO.File]::OpenRead($zip)
    try {
        $archive = New-Object IO.Compression.ZipArchive($zipStream, [IO.Compression.ZipArchiveMode]::Read, $true)
        try {
            $names = @{}
            $expandedBytes = 0L
            if ($archive.Entries.Count -gt 128) { throw 'Unexpected ZIP entry count.' }
            foreach ($entry in $archive.Entries) {
                $name = $entry.FullName
                # No directory entries are emitted by our packager. Reject
                # absolute paths, traversal, symlinks, duplicates and ADS names.
                $destination = Get-SafeChild $extractRoot $name
                $unixType = ($entry.ExternalAttributes -shr 16) -band 0xF000
                if (!$name.StartsWith("$packageName/", [StringComparison]::Ordinal) -or $name.Contains('\') -or
                    $names.ContainsKey($name) -or $unixType -eq 0xA000 -or ($entry.ExternalAttributes -band 0x400) -ne 0) { throw "Unsafe ZIP entry: $name" }
                $names[$name] = $true
                $expandedBytes += $entry.Length
                if ($expandedBytes -gt 1GB) { throw 'ZIP expanded size exceeds the portable demo limit.' }
                [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
                Assert-NoReparse $destination
                $inputStream = $entry.Open()
                $outputStream = [IO.File]::Open($destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose(); $inputStream.Dispose() }
            }
        } finally { $archive.Dispose() }
    } finally { $zipStream.Dispose() }
    $extracted = Get-SafeChild $extractRoot $packageName
    $null = Assert-Package $extracted
    $stageManifestHash = (Get-FileHash -LiteralPath (Get-SafeChild $stage 'manifest.json') -Algorithm SHA256).Hash
    if ($stageManifestHash -cne (Get-FileHash -LiteralPath (Get-SafeChild $extracted 'manifest.json') -Algorithm SHA256).Hash) { throw 'Staging and ZIP manifests differ.' }
    $report.manifestSHA256 = $stageManifestHash.ToLowerInvariant()
    $report.prepared = $true
    if (!$PrepareOnly) {
        foreach ($variant in @(@{ name = 'staging'; root = $stage }, @{ name = 'zip'; root = $extracted })) {
            # Keep isolated runtime paths short: classic Win32 upload file APIs
            # also append account/task hashes and a temporary-file suffix.
            $variantDirectory = if ($variant.name -ceq 'staging') { 'st' } else { 'zp' }
            $variantRoot = Get-SafeChild $runRoot $variantDirectory
            [IO.Directory]::CreateDirectory($variantRoot) | Out-Null
            # Real loopback startup and bounded protocol requests on OS-selected
            # ports. Probe completion is distinct from graceful service exit.
            foreach ($aux in @(@{ exe = 'native-service.exe'; probe = 'service' }, @{ exe = 'native-demo-http.exe'; probe = 'http' })) {
                $caseRoot = Get-SafeChild $variantRoot ([IO.Path]::GetFileNameWithoutExtension($aux.exe))
                $mediaRoot = Get-SafeChild $variant.root 'media'
                $auxArguments = if ($aux.probe -ceq 'service') { @('--port', '0', '--data', (Get-SafeChild $caseRoot 'data')) } else { @($mediaRoot, '0') }
                $record = Invoke-OwnedProcess (Get-SafeChild $variant.root $aux.exe) $auxArguments $caseRoot 30 $aux.probe $mediaRoot
                $record | Add-Member -NotePropertyName variant -NotePropertyValue $variant.name
                $report.tests += $record
                if (!$record.passed) { throw "Auxiliary executable failed: $($aux.exe)" }
            }
            foreach ($secondLayer in @($false, $true)) {
                $caseName = if ($secondLayer) { 'second-layer' } else { 'desktop-smoke' }
                $caseDirectory = if ($secondLayer) { 'sl' } else { 'ds' }
                $caseRoot = Get-SafeChild $variantRoot $caseDirectory
                $artifacts = Get-SafeChild $caseRoot 'a'
                $arguments = @('--decoder', 'ffmpeg', '--data-root', (Get-SafeChild $caseRoot 'data'))
                if ($secondLayer) { $arguments += @('--demo-fixtures', (Get-SafeChild $variant.root 'media'), '--second-layer-smoke', $artifacts) }
                else { $arguments += @('--source', (Get-SafeChild $variant.root 'media/stream.flv'), '--smoke-test', $artifacts) }
                $timeout = if ($secondLayer) { 125 } else { 40 }
                Write-Host "Running $($variant.name) $caseName (opens a player window; waits for exit)..."
                $record = Invoke-OwnedProcess (Get-SafeChild $variant.root 'native-player.exe') $arguments $caseRoot $timeout
                $record | Add-Member -NotePropertyName variant -NotePropertyValue $variant.name
                $record | Add-Member -NotePropertyName case -NotePropertyValue $caseName
                $report.tests += $record
                if (!$record.passed) { throw "$($variant.name) $caseName process failed or timed out." }
                try { $appResult = Assert-SmokeResult $artifacts $secondLayer }
                catch { $record.passed = $false; $record | Add-Member -NotePropertyName resultError -NotePropertyValue $_.Exception.Message; throw }
                $record | Add-Member -NotePropertyName appResult -NotePropertyValue $appResult
            }
        }
        $report.runtimeVerified = $true
    }
    # Detect source/release rewrites during verification, including after windows.
    $null = Assert-Package $stage
    $null = Assert-Package $extracted
    if ($stageManifestHash -cne (Get-FileHash -LiteralPath (Get-SafeChild $stage 'manifest.json') -Algorithm SHA256).Hash -or
        $zipHash -cne (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()) { throw 'Release changed during verification.' }
    $report.passed = $report.runtimeVerified
} catch { $report.error = $_.Exception.Message }
finally {
    $report.endedUtc = [DateTime]::UtcNow.ToString('o')
    Write-Json (Get-SafeChild $runRoot 'result.json') $report
}
if ($report.Contains('error')) { throw "Release verification failed: $($report.error). Evidence: $(Join-Path $runRoot 'result.json')" }
[pscustomobject]@{ Version = $Version; Prepared = $report.prepared; RuntimeVerified = $report.runtimeVerified; Passed = $report.passed; Result = (Join-Path $runRoot 'result.json'); Extracted = $extracted }

#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidatePattern('^3\.[0-9]+\.[0-9]+(?:-[a-z0-9]+(?:[.-][a-z0-9]+)*)?$')]
    [string]$Version = '3.0.0',
    [switch]$GenerateFixturesIfMissing,
    [switch]$IncludePerformance,
    [switch]$IncludeCommitMetadata
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Outputs are fixed children of this checkout. No caller-supplied output path,
# force-overwrite, recursive delete, or directory-wide input copy is accepted.
function Assert-NoReparse([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Reparse point rejected: $current"
            }
        }
        $parent = [IO.Directory]::GetParent($current)
        if ($null -eq $parent) { break }
        $current = $parent.FullName
    }
}
function Get-SafeChild([string]$Root, [string]$Relative) {
    if ([IO.Path]::IsPathRooted($Relative) -or $Relative -match '[:\x00-\x1f]') {
        throw "Rooted or invalid relative path: $Relative"
    }
    foreach ($part in ($Relative -split '[/\\]')) {
        if (!$part -or $part -in @('.', '..') -or $part -match '[ .]$|[<>"|?*]' -or
            $part -match '^(?i:CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.|$)') {
            throw "Unsafe path component: $Relative"
        }
    }
    $base = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $full = [IO.Path]::GetFullPath((Join-Path $base $Relative))
    if (!$full.StartsWith($base + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes allowed root: $Relative"
    }
    Assert-NoReparse $full
    return $full
}
function Assert-File([string]$Path) {
    Assert-NoReparse $Path
    if (!(Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -eq 0) {
        throw "Required nonempty file missing: $Path"
    }
}
function Write-Utf8([string]$Path, [string]$Text) {
    Assert-NoReparse $Path
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes($Text)
        $stream.Write($bytes, 0, $bytes.Length)
    } finally { $stream.Dispose() }
}

# Read-only PE32+ parsing; no dumpbin, SDK, loader or external executable.
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

$workspace = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
Assert-NoReparse $workspace
$packageName = "native-player-$Version-win-x64"
$releaseRoot = Get-SafeChild $workspace 'build/release'
$stage = Get-SafeChild $releaseRoot $packageName
$zipPath = Get-SafeChild $releaseRoot "$packageName.zip"
foreach ($path in @($stage, $zipPath, (Get-SafeChild $releaseRoot "$packageName.claim"))) {
    if (Test-Path -LiteralPath $path) { throw "Output is occupied; choose a new version, do not overwrite: $path" }
}

$inputs = New-Object 'Collections.Generic.List[object]'
function Add-Input([string]$Source, [string]$Destination, [string]$Role, [bool]$NormalizeEvidence = $false) {
    Assert-File $Source
    $null = Get-SafeChild $stage $Destination
    $inputs.Add([pscustomobject]@{ source = $Source; path = $Destination.Replace('\', '/'); role = $Role; normalizeEvidence = $NormalizeEvidence })
}
function Convert-EvidenceValue($Value, [string]$Field = '') {
    if ($null -eq $Value) { return $null }
    if ($Field -match '^(?i:password|token|username|account|serialnumber)(?:_|$)') { throw "Private evidence field rejected: $Field" }
    if ($Value -is [string]) {
        if ($Field -in @('command_line', 'executable_path')) {
            # Build-preflight process census may contain another user's tool
            # path or arguments. Retain times/CPU/PIDs, omit these text fields.
            return '[build-process path/arguments omitted; original evidence source SHA256 recorded]'
        }
        if ($Field -in @('source', 'raw_json', 'benchmark_executable', 'output_directory', 'path')) {
            if (!$Value) { return $Value }
            if (![IO.Path]::IsPathRooted($Value)) { throw "Expected workspace evidence path: $Field" }
            $full = [IO.Path]::GetFullPath($Value)
            if (!$full.StartsWith($workspace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw "Evidence path outside workspace: $Field" }
            $relative = $full.Substring($workspace.Length + 1).Replace('\', '/')
            $null = Get-SafeChild $workspace $relative
            return "workspace/$relative"
        }
        return $Value
    }
    if ($Value -is [Array]) {
        $converted = @($Value | ForEach-Object { Convert-EvidenceValue $_ $Field })
        return ,$converted
    }
    if ($Value -is [Collections.IDictionary] -or $Value -is [pscustomobject]) {
        $result = [ordered]@{}
        $names = if ($Value -is [Collections.IDictionary]) { @($Value.Keys) } else { @($Value.PSObject.Properties | ForEach-Object { $_.Name }) }
        foreach ($name in $names) { $result[$name] = Convert-EvidenceValue $Value.$name $name }
        return $result
    }
    if ($Value -is [Collections.IEnumerable]) {
        $converted = @($Value | ForEach-Object { Convert-EvidenceValue $_ $Field })
        return ,$converted
    }
    return $Value
}
$peEvidence = [ordered]@{}
foreach ($exe in @('native-player.exe', 'native-service.exe', 'native-demo-http.exe')) {
    $binary = Get-SafeChild $workspace "bin/desktop/Release/$exe"
    Add-Input $binary $exe 'binary'
    $peEvidence[$exe] = Assert-SystemImports $binary
}
$fixtures = Get-SafeChild $workspace 'build/demo-fixtures'
$fixtureNames = @('stream.flv', 'hls/index.m3u8') + @(0..7 | ForEach-Object { 'hls/segment-{0:D2}.ts' -f $_ })
$missing = @($fixtureNames | Where-Object { !(Test-Path -LiteralPath (Get-SafeChild $fixtures $_) -PathType Leaf) })
if ($missing.Count -gt 0) {
    if (!$GenerateFixturesIfMissing) { throw 'Demo fixtures missing; explicitly use -GenerateFixturesIfMissing after building make-stream-media.' }
    # The existing generator rewrites media. Never invoke it over a partial,
    # occupied fixture directory that another demo or test may still consume.
    if (Test-Path -LiteralPath $fixtures) { throw 'Partial/occupied demo fixture directory; generation refused to protect existing source identity.' }
    $generator = Get-SafeChild $workspace 'build/desktop/Release/make-stream-media.exe'
    Assert-File $generator
    & $generator $fixtures
    if ($LASTEXITCODE -ne 0) { throw "Fixture helper exited $LASTEXITCODE" }
}
$playlist = [IO.File]::ReadAllLines((Get-SafeChild $fixtures 'hls/index.m3u8'))
$references = @($playlist | Where-Object { $_ -and !$_.StartsWith('#') })
$expectedReferences = @(0..7 | ForEach-Object { 'segment-{0:D2}.ts' -f $_ })
if (($references -join '|') -cne ($expectedReferences -join '|') -or $playlist -notcontains '#EXT-X-ENDLIST') {
    throw 'Fixture playlist must be the existing eight-segment finite demo; external/rooted references are rejected.'
}
foreach ($name in $fixtureNames) { Add-Input (Get-SafeChild $fixtures $name) "media/$name" 'resource' }
foreach ($name in @('user-guide', 'architecture', 'demo', 'acceptance')) {
    Add-Input (Get-SafeChild $workspace "docs/delivery/$name.md") "docs/delivery/$name.md" 'documentation'
}
foreach ($name in @('docs/protocol/service-v1.md', 'docs/demo/second-layer.md', 'docs/validation/2026-10-13-first-layer.md', 'docs/validation/2026-10-13-second-layer.md')) {
    Add-Input (Get-SafeChild $workspace $name) $name 'documentation'
}
foreach ($name in @('docs/validation/2026-10-13-third-layer.md', 'docs/performance/method.md')) {
    $source = Get-SafeChild $workspace $name
    if (Test-Path -LiteralPath $source -PathType Leaf) { Add-Input $source $name 'documentation' }
}
$stabilityRoot = Get-SafeChild $workspace 'docs/validation/stability'
$stabilityNames = Get-StabilityNames
$stabilityCount = @($stabilityNames | Where-Object { Test-Path -LiteralPath (Get-SafeChild $stabilityRoot $_) }).Count
if ($stabilityCount -ne 0 -and $stabilityCount -ne 4) { throw 'Partial stability evidence; all four allowlisted files are required.' }
if ($stabilityCount -eq 4) {
    Assert-StabilityEvidence $stabilityRoot
    foreach ($name in $stabilityNames) { Add-Input (Get-SafeChild $stabilityRoot $name) "docs/validation/stability/$name" 'measurement' }
}
if ($IncludePerformance) {
    $evidenceRoot = Get-SafeChild $workspace 'build/third-layer/benchmark-results'
    $null = Assert-PerformanceEvidence $evidenceRoot -CheckSourceFiles
    $evidenceNames = Get-PerformanceNames
    foreach ($name in $evidenceNames) {
        $source = Get-SafeChild $evidenceRoot $name
        Add-Input $source "docs/performance/results/$name" 'measurement' ([IO.Path]::GetExtension($name) -in @('.json', '.csv'))
    }
}
Add-Input (Get-SafeChild $workspace 'third_party/imgui/LICENSE.txt') 'licenses/imgui-LICENSE.txt' 'license'

# Match the CMake dependency selection, including the historic nested layout.
$dependencyCandidates = @()
$cachePath = Get-SafeChild $workspace 'build/desktop/CMakeCache.txt'
if (Test-Path -LiteralPath $cachePath -PathType Leaf) {
    $line = @(Get-Content -LiteralPath $cachePath | Where-Object { $_ -match '^NP_DEPENDENCY_ROOT:PATH=.+$' })
    if ($line.Count -gt 0) {
        $configured = $line[0].Substring($line[0].IndexOf('=') + 1)
        $configured = [IO.Path]::GetFullPath($configured)
        if (!$configured.StartsWith($workspace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Configured dependency root is outside this workspace; supply licenses in the installed workspace layout.'
        }
        Assert-NoReparse $configured
        $dependencyCandidates += $configured
    }
}
$dependencyCandidates += @(Get-SafeChild $workspace 'vcpkg_installed/x64-windows-static'; Get-SafeChild $workspace 'vcpkg_installed/x64-windows-static/x64-windows-static')
$dependencyRoot = $null
foreach ($candidate in $dependencyCandidates) {
    if (Test-Path -LiteralPath (Get-SafeChild $candidate 'include/libavformat/avformat.h') -PathType Leaf) { $dependencyRoot = $candidate; break }
}
if (!$dependencyRoot) { throw 'Installed static dependency root not found; actual copyright files are required.' }
Add-Input (Get-SafeChild $dependencyRoot 'share/ffmpeg/copyright') 'licenses/ffmpeg-copyright.txt' 'license'
Add-Input (Get-SafeChild $dependencyRoot 'share/nlohmann-json/copyright') 'licenses/nlohmann-json-copyright.txt' 'license'
$dependencyIdentity = [ordered]@{
    installedRoot = $dependencyRoot.Substring($workspace.Length + 1).Replace('\', '/')
    ffmpegVersion = '6.1.1'; ffmpegPortVersion = 11; nlohmannJsonVersion = '3.12.0'; imguiTag = 'v1.91.9b'
    selection = 'CMake NP_DEPENDENCY_ROOT, otherwise first installed candidate with libavformat headers.'
    staticLibraries = @('avformat', 'avcodec', 'avutil', 'swresample', 'swscale'); runtime = 'MSVC /MT'
    configureFlags = $null
}
# The configuration function can be removed by linker dead-code elimination
# from the EXE. Inspect the exact avutil.lib selected for Release as fallback.
$configSource = Get-SafeChild $workspace 'bin/desktop/Release/native-player.exe'
function Get-ConfigureStrings([string]$Path) {
    $ascii = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($Path))
    return @([regex]::Matches($ascii, '[\x20-\x7e]{16,}') | ForEach-Object { $_.Value } |
        Where-Object { $_ -match '--toolchain=msvc' -and $_ -match '--enable-avcodec' -and $_ -match '--enable-avformat' })
}
$configStrings = @(Get-ConfigureStrings $configSource)
if ($configStrings.Count -ne 1) {
    $configSource = Get-SafeChild $dependencyRoot 'lib/avutil.lib'
    Assert-File $configSource
    $configStrings = @(Get-ConfigureStrings $configSource)
}
if ($configStrings.Count -eq 1) {
    $rawConfig = $configStrings[0]
    # Keep actual feature flags; replace only historic builder-local tool paths.
    $safeConfig = [regex]::Replace($rawConfig, "--(prefix|pkg-config|extra-ldflags)=(?:'[^']*'|[^ ]+)", '--$1=<builder-path-omitted>')
    $dependencyIdentity.configureFlags = $safeConfig
    $dependencyIdentity.configurationSource = $configSource.Substring($workspace.Length + 1).Replace('\', '/')
    $dependencyIdentity.configurationSourceSHA256 = (Get-FileHash -LiteralPath $configSource -Algorithm SHA256).Hash.ToLowerInvariant()
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $dependencyIdentity.rawConfigureStringSHA256 = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::ASCII.GetBytes($rawConfig)))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
    $dependencyIdentity.configurationEvidence = 'Actual retained configure string; only prefix/pkg-config/extra-ldflags builder paths omitted. Source and raw-string SHA256 recorded.'
} else { $dependencyIdentity.configurationEvidence = 'Linked configure string not uniquely retained; installed ABI identity provided, no flag reconstruction.' }
$abiPath = Get-SafeChild $dependencyRoot 'share/ffmpeg/vcpkg_abi_info.txt'
if (Test-Path -LiteralPath $abiPath -PathType Leaf) {
    $dependencyIdentity.abiFileSHA256 = (Get-FileHash -LiteralPath $abiPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $dependencyIdentity.installedFeatures = @(Get-Content -LiteralPath $abiPath | Where-Object { $_ -match '^features ' }) -join ''
}

$commit = $null
if ($IncludeCommitMetadata) {
    $git = Get-Command git -CommandType Application -ErrorAction Stop
    $hash = & $git.Source -C $workspace rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $hash -notmatch '^[a-f0-9]{40,64}$') { throw 'Could not read commit identity.' }
    $status = & $git.Source -C $workspace status --porcelain
    if ($LASTEXITCODE -ne 0) { throw 'Could not read working-tree status.' }
    $commit = [ordered]@{ head = "$hash"; workingTreeDirty = [bool]$status; note = 'HEAD describes source history, not a clean-build attestation.' }
}
[IO.Directory]::CreateDirectory($releaseRoot) | Out-Null
Assert-NoReparse $releaseRoot
# A permanent claim is intentional: incomplete output remains reviewable and
# requires a new version, rather than risky cleanup of an occupied directory.
Write-Utf8 (Get-SafeChild $releaseRoot "$packageName.claim") "Reserved by package-release.ps1 at $([DateTime]::UtcNow.ToString('o'))`r`n"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $zipPath)) { throw 'Output appeared during reservation; refusing overwrite.' }
[IO.Directory]::CreateDirectory($stage) | Out-Null
$entries = New-Object 'Collections.Generic.List[object]'
foreach ($inputFile in $inputs) {
    $destination = Get-SafeChild $stage $inputFile.path
    [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
    Assert-NoReparse $destination
    $before = (Get-FileHash -LiteralPath $inputFile.source -Algorithm SHA256).Hash
    if ($inputFile.normalizeEvidence) {
        if ([IO.Path]::GetExtension($inputFile.path) -eq '.json') {
            $value = [IO.File]::ReadAllText($inputFile.source) | ConvertFrom-Json
            $converted = Convert-EvidenceValue $value
            Write-Utf8 $destination (ConvertTo-Json -InputObject $converted -Depth 32)
        } else {
            $rows = @(Import-Csv -LiteralPath $inputFile.source | ForEach-Object { [pscustomobject](Convert-EvidenceValue $_) })
            Write-Utf8 $destination (($rows | ConvertTo-Csv -NoTypeInformation) -join "`r`n")
        }
    } else { [IO.File]::Copy($inputFile.source, $destination, $false) }
    $after = (Get-FileHash -LiteralPath $inputFile.source -Algorithm SHA256).Hash
    $copied = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
    if ($before -cne $after -or (!$inputFile.normalizeEvidence -and $before -cne $copied)) { throw "Input changed during packaging: $($inputFile.path)" }
    $manifestEntry = [ordered]@{ path = $inputFile.path; role = $inputFile.role; bytes = (Get-Item -LiteralPath $destination).Length; sha256 = $copied.ToLowerInvariant() }
    if ($inputFile.normalizeEvidence) {
        $manifestEntry.sourceSHA256 = $before.ToLowerInvariant()
        $manifestEntry.normalization = 'Workspace paths become relative labels; build-process path/argument text omitted; numeric observations retained.'
    }
    $entries.Add($manifestEntry)
}
foreach ($exe in @('native-player.exe', 'native-service.exe', 'native-demo-http.exe')) {
    $actual = Assert-SystemImports (Get-SafeChild $stage $exe)
    if (($actual | ConvertTo-Json -Compress -Depth 4) -cne ($peEvidence[$exe] | ConvertTo-Json -Compress -Depth 4)) { throw 'PE dependencies changed while packaging.' }
}
if ($IncludePerformance) { $null = Assert-PerformanceEvidence (Get-SafeChild $stage 'docs/performance/results') }
if ($stabilityCount -eq 4) { Assert-StabilityEvidence (Get-SafeChild $stage 'docs/validation/stability') }

$launchDemo = @'
@echo off
setlocal DisableDelayedExpansion
set "NP_DATA=%~dp0portable-data"
if not exist "%NP_DATA%\" mkdir "%NP_DATA%" 2>nul
set "NP_PROBE=%NP_DATA%\.write-probe-%RANDOM%-%RANDOM%"
(>"%NP_PROBE%" echo probe) 2>nul
if exist "%NP_PROBE%" goto data_ready
set "NP_DATA=%LOCALAPPDATA%\NativePlayer\portable-__VERSION__"
if not defined LOCALAPPDATA goto data_error
if not exist "%NP_DATA%\" mkdir "%NP_DATA%" 2>nul
set "NP_PROBE=%NP_DATA%\.write-probe-%RANDOM%-%RANDOM%"
(>"%NP_PROBE%" echo probe) 2>nul
if not exist "%NP_PROBE%" goto data_error
:data_ready
del /q "%NP_PROBE%" >nul 2>&1
"%~dp0native-player.exe" --decoder ffmpeg --demo-fixtures "%~dp0media" --data-root "%NP_DATA%"
set "NP_EXIT=%ERRORLEVEL%"
if "%NP_EXIT%"=="0" exit /b 0
echo Player exited with code %NP_EXIT%. See docs\delivery\user-guide.md.
pause
exit /b %NP_EXIT%
:data_error
echo Cannot create writable demo data. Extract to a writable folder and retry.
pause
exit /b 1
'@
$launchNormal = @'
@echo off
setlocal DisableDelayedExpansion
"%~dp0native-player.exe" --decoder ffmpeg
set "NP_EXIT=%ERRORLEVEL%"
if "%NP_EXIT%"=="0" exit /b 0
echo Player exited with code %NP_EXIT%. See docs\delivery\user-guide.md.
pause
exit /b %NP_EXIT%
'@
$notice = @'
# Installed third-party notices

The package copies these actual files without alteration:
- Dear ImGui: third_party/imgui/LICENSE.txt (MIT).
- FFmpeg: installed share/ffmpeg/copyright (LGPL text from the installed port).
- nlohmann-json: installed share/nlohmann-json/copyright (MIT).

Libraries and the MSVC runtime are linked statically by this project's build.
manifest.json records the installed dependency root, ABI identity and actual
configure flags from the EXE or selected Release avutil.lib, when retained.
Only historic builder tool paths are omitted; their raw-string hash is recorded.
Only license documents are included here; this package does not include
dependency sources, a fulfilled corresponding-source offer, or relinking
object files. A later distributor must address the applicable LGPL static-link
source/relinking requirements and an appropriate source-delivery/offer route.
Copying these notices alone does not establish redistribution compliance.
Windows fonts and system DLLs are not copied from this machine.
'@
foreach ($generated in @(
    @{ path = 'launch-demo.cmd'; text = $launchDemo.Replace('__VERSION__', $Version); role = 'launcher' },
    @{ path = 'launch.cmd'; text = $launchNormal; role = 'launcher' },
    @{ path = 'licenses/NOTICE.md'; text = $notice; role = 'license' }
)) {
    $path = Get-SafeChild $stage $generated.path
    Write-Utf8 $path ($generated.text.Replace("`r`n", "`n").Replace("`n", "`r`n") + "`r`n")
    $entries.Add([ordered]@{ path = $generated.path; role = $generated.role; bytes = (Get-Item -LiteralPath $path).Length; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() })
}
$manifest = [ordered]@{
    schemaVersion = 3; version = $Version; packageName = $packageName
    platform = 'windows-x64'; configuration = 'Release'; createdUtc = [DateTime]::UtcNow.ToString('o')
    hashAlgorithm = 'SHA256'; files = @($entries.ToArray()); dependencies = $dependencyIdentity; peImports = $peEvidence
    excluded = @('personal media', 'accounts', 'sessions', 'tokens', 'runtime data', 'build tools', 'dependency sources')
    verification = 'Not performed by packaging. Run verify-release.ps1 after final binaries are ready.'
}
if ($commit) { $manifest.commit = $commit }
Write-Utf8 (Get-SafeChild $stage 'manifest.json') ($manifest | ConvertTo-Json -Depth 8)
Add-Type -AssemblyName System.IO.Compression
$zipStream = [IO.File]::Open($zipPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    $archive = New-Object IO.Compression.ZipArchive($zipStream, [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        foreach ($relative in @($entries.ToArray() | ForEach-Object { $_.path }) + @('manifest.json')) {
            $entry = $archive.CreateEntry("$packageName/$relative", [IO.Compression.CompressionLevel]::Optimal)
            $sourceStream = [IO.File]::OpenRead((Get-SafeChild $stage $relative))
            $entryStream = $entry.Open()
            try { $sourceStream.CopyTo($entryStream) } finally { $entryStream.Dispose(); $sourceStream.Dispose() }
        }
    } finally { $archive.Dispose() }
} finally { $zipStream.Dispose() }
[pscustomobject]@{ Version = $Version; Stage = $stage; Zip = $zipPath; Manifest = (Join-Path $stage 'manifest.json'); ZipSHA256 = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash; Files = $entries.Count }

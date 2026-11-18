<#
Runs the NAX5 stream replay matrix against a replay-enabled chiaki.exe
(configured with -DNAX5_STREAM_REPLAY=ON) and fails on any crash, hang or
starved decoder.

  run-stream-replay.ps1 -ChiakiExe <build>\gui\chiaki.exe -MediaRoot <dir from make-replay-media.py>
      [-Scenario name[,name...]] [-Repeat 3] [-DllDir dir[,dir...]] [-OutDir dir]

Every scenario runs in operator mode (separate NAX5-Operator profile) against
a dead API URL, so nothing reaches production. Crash dumps written by the
client's crash handler are collected per run.
#>
param(
    [Parameter(Mandatory = $true)][string]$ChiakiExe,
    [Parameter(Mandatory = $true)][string]$MediaRoot,
    [string[]]$Scenario = @(),
    [int]$Repeat = 1,
    [string[]]$DllDir = @(),
    [string]$SoftwareOpenGLDir = '',
    [switch]$DummyAudio,
    [string]$OutDir = (Join-Path $env:TEMP ('nax5-replay-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
)
$ErrorActionPreference = 'Stop'

# name, media, seconds, decoder, mic, echo, loss%, corrupt%, burst every s, stall ms, needs_audio
$matrix = @(
    @{ n='h264-720p60-auto';        m='h264-720p60';     s=20; dec='auto';    mic=0; echo=0 },
    @{ n='h264-720p60-software';    m='h264-720p60';     s=20; dec='none';    mic=0; echo=0 },
    @{ n='h264-720p60-d3d11va';     m='h264-720p60';     s=20; dec='d3d11va'; mic=0; echo=0 },
    @{ n='h264-720p60-vulkan';      m='h264-720p60';     s=20; dec='vulkan';  mic=0; echo=0 },
    @{ n='h264-1080p60-auto';       m='h264-1080p60';    s=20; dec='auto';    mic=0; echo=0 },
    @{ n='h264-720p30-auto';        m='h264-720p30';     s=20; dec='auto';    mic=0; echo=0 },
    @{ n='h265-1080p60-auto';       m='h265-1080p60';    s=20; dec='auto';    mic=0; echo=0 },
    @{ n='h265hdr-1080p60-auto';    m='h265hdr-1080p60'; s=20; dec='auto';    mic=0; echo=0 },
    @{ n='mic-on';                  m='h264-720p60';     s=20; dec='auto';    mic=1; echo=0 },
    @{ n='mic-on-echo';             m='h264-720p60';     s=20; dec='auto';    mic=1; echo=1 },
    @{ n='mic-on-echo-no-playback'; m='h264-720p60';     s=20; dec='auto';    mic=1; echo=1; header_only=1 },
    @{ n='loss-2pct';               m='h264-720p60';     s=20; dec='auto';    mic=0; echo=0; loss=2 },
    @{ n='loss-10pct';              m='h264-720p60';     s=20; dec='auto';    mic=0; echo=0; loss=10 },
    @{ n='corrupt-3pct';            m='h264-720p60';     s=20; dec='auto';    mic=0; echo=0; corrupt=3 },
    @{ n='jitter-bursts';           m='h264-720p60';     s=20; dec='auto';    mic=0; echo=0; burst=2 },
    @{ n='stall-3s';                m='h264-720p60';     s=20; dec='auto';    mic=0; echo=0; stall=3000 },
    @{ n='h265-loss-corrupt';       m='h265-1080p60';    s=20; dec='auto';    mic=0; echo=0; loss=5; corrupt=2 }
)
$paired = @(
    @{ n='rcp-clean-queued';    m='h265-1080p60-30content'; s=6; dec='none'; input=1; pull=0 },
    @{ n='rcp-clean-pull';      m='h265-1080p60-30content'; s=6; dec='none'; input=1; pull=1 },
    @{ n='rcp-impaired-queued'; m='h265-1080p60-30content'; s=6; dec='none'; input=1; pull=0; loss=3; audio_loss=2; burst=2; hold=80 },
    @{ n='rcp-impaired-pull';   m='h265-1080p60-30content'; s=6; dec='none'; input=1; pull=1; loss=3; audio_loss=2; burst=2; hold=80 }
)
# Existing hardware/legacy scenarios remain the default for local runs.
if ($Scenario.Count -gt 0) { $matrix += $paired }
if ($Scenario.Count -gt 0) { $matrix = $matrix | Where-Object { $Scenario -contains $_.n } }
if (-not $matrix) { throw 'no scenario selected' }
foreach ($name in $Scenario) {
    if ($name -notin $matrix.n) { throw "unknown replay scenario '$name'" }
}
if ($Repeat -lt 1) { throw 'Repeat must be positive' }
$MediaRoot = (Resolve-Path -LiteralPath $MediaRoot).Path

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
$chiakiDir = Split-Path -Parent (Resolve-Path $ChiakiExe)
$profileDir = Join-Path $env:APPDATA 'NAX5\RCP-Operator\log'
$dumpDir = Join-Path $profileDir 'crash-dumps'
$results = @()

foreach ($sc in $matrix) {
    foreach ($run in 1..$Repeat) {
        $tag = "$($sc.n)#$run"
        $resultPath = Join-Path $OutDir ("$($sc.n)-$run.json")
        if (Test-Path -LiteralPath $resultPath) { Remove-Item -LiteralPath $resultPath }
        $dumpsBefore = @(if (Test-Path $dumpDir) { Get-ChildItem $dumpDir -Filter *.txt | ForEach-Object Name })
        $started = Get-Date

        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = (Resolve-Path $ChiakiExe).Path
        $psi.WorkingDirectory = $chiakiDir
        $psi.UseShellExecute = $false
        $psi.RedirectStandardError = $true
        $psi.EnvironmentVariables['PATH'] = (($DllDir + @($chiakiDir, "$env:SystemRoot\System32", $env:SystemRoot)) -join ';')
        if ($SoftwareOpenGLDir) {
            $mesa = (Resolve-Path -LiteralPath $SoftwareOpenGLDir).Path
            $psi.EnvironmentVariables['PATH'] = "$mesa;$($psi.EnvironmentVariables['PATH'])"
            $psi.EnvironmentVariables['QT_OPENGL'] = 'software'
            $psi.EnvironmentVariables['QT_OPENGL_DLL'] = (Join-Path $mesa 'opengl32.dll')
            $psi.EnvironmentVariables['GALLIUM_DRIVER'] = 'llvmpipe'
            $psi.EnvironmentVariables['CHIAKI_FORCE_VULKAN_FALLBACK'] = '1'
        }
        if ($DummyAudio) {
            $psi.EnvironmentVariables['SDL_AUDIODRIVER'] = 'dummy'
            $psi.EnvironmentVariables['SDL_AUDIO_DRIVER'] = 'dummy'
        }
        $vars = @{
            NAX5_OPERATOR_MODE = '1'; NAX5_API_BASE_URL = 'http://127.0.0.1:9'
            NAX5_REPLAY_DIR = (Join-Path $MediaRoot $sc.m); NAX5_REPLAY_SECONDS = "$($sc.s)"
            NAX5_REPLAY_DECODER = $sc.dec; NAX5_REPLAY_MIC = "$($sc.mic)"; NAX5_REPLAY_ECHO = "$($sc.echo)"
            NAX5_REPLAY_LOSS_PCT = "$([int]$sc.loss)"; NAX5_REPLAY_CORRUPT_PCT = "$([int]$sc.corrupt)"
            NAX5_REPLAY_BURST_EVERY_S = "$([int]$sc.burst)"; NAX5_REPLAY_STALL_MS = "$([int]$sc.stall)"
            NAX5_REPLAY_AUDIO_LOSS_PCT = "$([int]$sc.audio_loss)"
            NAX5_REPLAY_BURST_HOLD_MS = "$(if ($sc.hold) { $sc.hold } else { 120 })"
            NAX5_REPLAY_INPUT = "$([int]$sc.input)"; NAX5_REPLAY_SEED = '2406'
            RCP_AUDIO_PULL = "$([int]$sc.pull)"
            NAX5_REPLAY_RESULT = $resultPath
            NAX5_REPLAY_AUDIO_HEADER_ONLY = "$([int]$sc.header_only)"
        }
        foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = $vars[$k] }

        $proc = [System.Diagnostics.Process]::Start($psi)
        $detail = ''
        $stderr = $proc.StandardError.ReadToEndAsync()
        $finished = $proc.WaitForExit(($sc.s + 45) * 1000)
        if (-not $finished) { $proc.Kill(); $proc.WaitForExit(5000) | Out-Null }
        if ($stderr.IsCompleted -and -not $stderr.IsFaulted -and -not $stderr.IsCanceled) {
            $detail = $stderr.GetAwaiter().GetResult()
            if ($detail) { Set-Content -LiteralPath (Join-Path $OutDir "$($sc.n)-$run.stderr.txt") -Value $detail }
        }
        Start-Sleep -Milliseconds 150

        $r = if (Test-Path $resultPath) { Get-Content $resultPath -Raw | ConvertFrom-Json } else { $null }
        $newDumps = @(if (Test-Path $dumpDir) { Get-ChildItem $dumpDir -Filter *.txt | Where-Object { $dumpsBefore -notcontains $_.Name } })
        $log = if (Test-Path -LiteralPath $profileDir) {
            Get-ChildItem $profileDir -Filter 'chiaki_session_*.log' | Where-Object { $_.LastWriteTime -ge $started } |
                Sort-Object LastWriteTime | Select-Object -Last 1
        }
        $logText = if ($log) { Get-Content $log.FullName -Raw } else { '' }
        $decoderLine = ([regex]::Match($logText, 'Using (hardware decoder "[^"]+"|software decoder[^\r\n]*)')).Value
        $micFailed = $logText -match 'Failed to open (default )?Microphone'

        $problems = @()
        if (-not $finished) { $problems += 'hang' }
        elseif ($proc.ExitCode -ne 0) { $problems += ('exit 0x{0:X8}' -f $proc.ExitCode) }
        if ($newDumps.Count -gt 0) { $problems += "crash dump x$($newDumps.Count)" }
        if (-not $r) { $problems += 'no result' }
        elseif ($r.outcome -ne 'completed') { $problems += "outcome $($r.outcome)" }
        else {
            $expected = [math]::Max(1, $r.video_sent - $r.video_rejected_by_decoder)
            $ratio = $r.decoded_frames / $expected
            $floor = if ($sc.input -or $sc.loss -or $sc.corrupt) { 0.5 } else { 0.95 }
            if ($ratio -lt $floor) { $problems += ('decoded {0:P0} of sent' -f $ratio) }
            if (($r.audio_sent -gt 0 -or $sc.header_only) -and -not $r.audio_output.opened) { $problems += 'audio device not opened' }
            if ($sc.mic -eq 1 -and -not $micFailed -and $r.mic_frames -lt 100) { $problems += "mic frames $($r.mic_frames)" }
            if ($sc.input) {
                $mode = if ($sc.pull) {'pull'} else {'queued'}
                if ($r.audio_output.mode -ne $mode) { $problems += "expected $mode audio, got $($r.audio_output.mode)" }
                if ($r.audio_sent + $r.audio_dropped -ne 600 -or $r.video_sent + $r.video_dropped -ne 360) {
                    $problems += 'incomplete media timeline'
                }
                if ($r.input_sent -ne 240 -or $r.input_ok -ne $r.input_sent -or $r.input_errors -ne 0) {
                    $problems += 'missing or incorrect mapped input'
                }
                if ($r.video_submitted -le 0) { $problems += 'no successful video submissions' }
                if ($sc.burst -and (-not $r.video_held -or -not $r.audio_held -or -not $r.input_held -or
                                    -not $r.video_dropped -or -not $r.audio_dropped)) {
                    $problems += 'impaired trace did not exercise all lanes'
                }
            }
        }
        foreach ($d in $newDumps) {
            Copy-Item $d.FullName -Destination (Join-Path $OutDir "$($sc.n)-$run-$($d.Name)")
            $dmp = [IO.Path]::ChangeExtension($d.FullName, '.dmp')
            if (Test-Path $dmp) { Move-Item $dmp -Destination (Join-Path $OutDir "$($sc.n)-$run-$([IO.Path]::GetFileName($dmp))") -Force }
            Remove-Item $d.FullName -Force
        }
        if ($log) { Copy-Item $log.FullName -Destination (Join-Path $OutDir "$($sc.n)-$run.log") }

        $row = [pscustomobject]@{
            scenario = $tag
            result = if ($problems) { 'FAIL' } else { 'PASS' }
            sent = $r.video_sent; decoded = $r.decoded_frames; dropped = $r.video_dropped
            held_video = $r.video_held; held_audio = $r.audio_held; held_input = $r.input_held
            audio = $r.audio_sent; audio_dropped = $r.audio_dropped; input = $r.input_ok
            video_submitted = $r.video_submitted
            frame_ready_gap_max_us = $r.frame_ready_gap_max_us; input_dispatch_max_us = $r.input_dispatch_max_us
            audio_mode = $r.audio_output.mode; pull_underruns = $r.audio_output.pull_underruns
            pull_busy_callbacks = $r.audio_output.pull_busy_callbacks
            pull_discarded_pcm_bytes = $r.audio_output.pull_discarded_pcm_bytes
            mic = if ($sc.mic -eq 1 -and $micFailed) { 'no-device' } else { $r.mic_frames }
            decoder = $decoderLine -replace 'Using ', ''
            note = if ($sc.dec -notin @('auto', 'none') -and $decoderLine -notmatch [regex]::Escape($sc.dec)) { "requested $($sc.dec) unavailable here, fell back" } else { '' }
            problems = ($problems -join '; ')
        }
        $results += $row
        if ($problems -and $detail) {
            Write-Host (($detail -split '\r?\n' | Select-Object -Last 25) -join [Environment]::NewLine)
        }
        Write-Host ('{0,-28} {1}  sent={2} decoded={3} audio={4} mic={5} {6} {7} {8}' -f $row.scenario, $row.result, $row.sent, $row.decoded, $row.audio, $row.mic, $row.decoder, $row.problems, $row.note)
    }
}

# Check that each A/B pair received the same seeded inputs, not merely a
# similar media length. Timing and audio backlog values are measurements,
# not equality gates.
foreach ($kind in @('clean', 'impaired')) {
    foreach ($run in 1..$Repeat) {
        $queued = $results | Where-Object scenario -eq "rcp-$kind-queued#$run"
        $pull = $results | Where-Object scenario -eq "rcp-$kind-pull#$run"
        if ($queued -and $pull) {
            foreach ($key in @('sent', 'dropped', 'audio', 'audio_dropped', 'input', 'held_video', 'held_audio', 'held_input')) {
                if ($queued.$key -ne $pull.$key) {
                    $pull.result = 'FAIL'
                    $pull.problems = "$($pull.problems); paired timeline differs for $key"
                }
            }
        }
    }
}
if ($env:GITHUB_STEP_SUMMARY -and ($results.scenario -like 'rcp-*')) {
    $summary = @('### Offline RCP stream replay', '',
        '| Scenario | Gate | Decoded | Video submissions | AU dropped | Opus dropped | Mapped inputs | Max frame-ready gap (ms) | Max GUI input dispatch (ms) | Pull underruns |',
        '|---|---|---:|---:|---:|---:|---:|---:|---:|---:|')
    $summary += foreach ($row in $results) {
        '| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7:F1} | {8:F1} | {9} |' -f $row.scenario, $row.result, $row.decoded, $row.video_submitted,
            $row.dropped, $row.audio_dropped, $row.input, ($row.frame_ready_gap_max_us / 1000),
            ($row.input_dispatch_max_us / 1000), $(if ($row.audio_mode -eq 'pull') { $row.pull_underruns } else { 'n/a' })
    }
    $summary += @('', 'Software OpenGL, software decoding and SDL dummy audio. Pre-decoder loss and GUI dispatch measurements only.')
    $summary | Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY
}
Write-Host ('Replay evidence: ' + (ConvertTo-Json -InputObject @($results) -Depth 3 -Compress))
$results | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $OutDir 'summary.json') -Encoding utf8
$failed = @($results | Where-Object result -eq 'FAIL').Count
Write-Host "`n$($results.Count - $failed)/$($results.Count) passed. Artifacts: $OutDir"
if ($failed -gt 0) { exit 1 }
exit 0

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '../..')

function Read-Source([string]$Path) {
    Get-Content -LiteralPath (Join-Path $root $Path) -Raw -Encoding UTF8
}

$settings = Read-Source 'gui/src/qmlsettings.cpp'
$preset = [regex]::Match($settings, '(?s)void QmlSettings::applyRcpRemotePreset\(bool stability\)\s*\{(.*?)\}').Groups[1].Value
foreach ($rule in @(
    'setResolutionRemotePS5\(stability\s*\?\s*CHIAKI_VIDEO_RESOLUTION_PRESET_720p\s*:\s*CHIAKI_VIDEO_RESOLUTION_PRESET_1080p\)',
    'setFpsRemotePS5\(CHIAKI_VIDEO_FPS_PRESET_60\)',
    'setCodecRemotePS5\(CHIAKI_CODEC_H265\)',
    'setBitrateRemotePS5\(stability \? 10000 : 15000\)'
)) {
    if ($preset -notmatch $rule) { throw "Unexpected RCP PS5 preset: $rule" }
}
if ([regex]::Matches($preset, '\bset\w+\(').Count -ne 4) { throw 'Presets must leave unrelated settings alone' }

$panel = Read-Source 'gui/src/qml/Nax5PlayPanel.qml'
if ($panel -notmatch 'applyRcpRemotePreset\(false\)' -or $panel -notmatch 'applyRcpRemotePreset\(true\)') {
    throw 'Both presets must be wired to the play panel'
}
if ($panel -notmatch 'showStreamStats = checked' -or $panel -notmatch 'Nax5Session.play\(\)') {
    throw 'The diagnostics toggle and the original Play action must remain available'
}

$player = Read-Source 'gui/src/qml/StreamView.qml'
if ([regex]::Matches($player, 'sourceComponent: streamStatsContent').Count -ne 2 -or
    $player -notmatch 'Chiaki.window.localDroppedFrames' -or
    $player -notmatch 'session.framesLost' -or
    $player -notmatch 'session.decoderBackend') {
    throw 'Both overlays must show stream loss, local drops and the initialized decoder'
}
if ($player -match 'Chiaki.window.droppedFrames\b') {
    # The original menu may still use the combined counter, but the diagnostic overlay must not.
    $overlay = [regex]::Match($player, '(?s)id: streamStatsContent\s*.*?(?=\n    Item \{\s*id: streamStatsInline)').Value
    if ($overlay -match 'Chiaki.window.droppedFrames\b') { throw 'RCP overlay conflates incoming and local losses' }
}

$runtime = Read-Source 'gui/src/nax5/nax5runtime.cpp'
$instance = Read-Source 'gui/src/nax5/nax5singleinstance.cpp'
if ($runtime -notmatch 'QStringLiteral\("RCP"\)' -or $instance -notmatch 'RCP-client-') {
    throw 'RCP must have its own settings and single-instance namespace'
}
Write-Host 'RCP preset, overlay, and local settings contracts passed'

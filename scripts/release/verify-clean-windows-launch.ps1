param(
    [Parameter(Mandatory = $true)]
    [string]$BundleDir,
    [string]$InstallerExe = '',
    [string]$SoftwareOpenGLDir = '',
    [int]$SettleSeconds = 8
)

$ErrorActionPreference = 'Stop'

function Stop-ChiakiIfRunning {
    Get-Process -Name chiaki -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
}

function Test-ChiakiLaunch {
    param([Parameter(Mandatory = $true)][string]$AppDir)

    $chiaki = Join-Path $AppDir 'chiaki.exe'
    if (-not (Test-Path -LiteralPath $chiaki)) {
        throw "chiaki.exe missing in $AppDir"
    }
    $ffmpeg = @(
        (Get-ChildItem -LiteralPath $AppDir -Filter 'avutil-*.dll'),
        (Get-ChildItem -LiteralPath $AppDir -Filter 'avcodec-*.dll'),
        (Get-ChildItem -LiteralPath $AppDir -Filter 'avformat-*.dll'),
        (Get-ChildItem -LiteralPath $AppDir -Filter 'swresample-*.dll')
    ) | Where-Object { $_ }
    if ($ffmpeg.Count -lt 4) {
        throw "FFmpeg DLLs missing next to chiaki.exe in $AppDir"
    }

    Stop-ChiakiIfRunning

    $systemRoot = $env:SystemRoot
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $chiaki
    $psi.WorkingDirectory = $AppDir
    $psi.UseShellExecute = $false
    $psi.EnvironmentVariables['PATH'] = "$AppDir;$systemRoot\System32;$systemRoot"
    foreach ($name in @('MSYSTEM', 'MSYS', 'MINGW_PREFIX', 'MSYSTEM_PREFIX', 'PKG_CONFIG_PATH', 'CHERE_INVOKING')) {
        $psi.EnvironmentVariables.Remove($name)
    }

    $keys = @()
    foreach ($entry in $psi.EnvironmentVariables.GetEnumerator()) {
        $keys += $entry.Key
    }
    foreach ($key in $keys) {
        if ($key -eq 'PATH') { continue }
        $value = $psi.EnvironmentVariables[$key]
        if ($value -and $value -match 'msys64') {
            $psi.EnvironmentVariables.Remove($key)
        }
    }

    if ($SoftwareOpenGLDir) {
        # First check DLL loading with the clean bundle PATH, without any Mesa DLLs.
        $probe = New-Object System.Diagnostics.Process
        $probe.StartInfo = $psi
        $probe.StartInfo.Arguments = 'list'
        if (-not $probe.Start()) { throw "could not launch dependency probe" }
        if (-not $probe.WaitForExit(15000)) {
            $probe.Kill()
            throw "clean dependency probe timed out"
        }
        if ($probe.ExitCode -ne 0) {
            throw "clean dependency probe failed with exit code $($probe.ExitCode)"
        }
        $probe.Dispose()
        $psi.Arguments = ''

        # Only the isolated CI GUI smoke needs Mesa. Do not add it to the ZIP.
        $mesa = (Resolve-Path -LiteralPath $SoftwareOpenGLDir).Path
        $psi.EnvironmentVariables['PATH'] = "$AppDir;$mesa;$systemRoot\System32;$systemRoot"
        $psi.EnvironmentVariables['QT_OPENGL'] = 'software'
        $psi.EnvironmentVariables['QT_OPENGL_DLL'] = (Join-Path $mesa 'opengl32.dll')
        $psi.EnvironmentVariables['GALLIUM_DRIVER'] = 'llvmpipe'
        $psi.EnvironmentVariables['CHIAKI_FORCE_VULKAN_FALLBACK'] = '1'
    }

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    if (-not $proc.Start()) {
        throw "failed to start $chiaki"
    }

    Start-Sleep -Seconds $SettleSeconds
    if ($proc.HasExited) {
        $logDir = Join-Path $env:APPDATA 'NAX5\RCP\log'
        if (Test-Path -LiteralPath $logDir) {
            Get-ChildItem -LiteralPath $logDir -Filter 'nax5_*.log' |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1 |
                ForEach-Object { Get-Content -LiteralPath $_.FullName -Tail 60 }
        }
        throw "chiaki.exe exited immediately with code $($proc.ExitCode) from $AppDir"
    }

    Stop-Process -Id $proc.Id -Force
    try {
        $proc.WaitForExit(10000) | Out-Null
    } catch {
        Stop-ChiakiIfRunning
    }
    Write-Host "CLEAN_LAUNCH_OK dir=$AppDir pid=$($proc.Id)"
}

$BundleDir = (Resolve-Path -LiteralPath $BundleDir).Path
Test-ChiakiLaunch -AppDir $BundleDir

if ($InstallerExe) {
    $InstallerExe = (Resolve-Path -LiteralPath $InstallerExe).Path
    $info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($InstallerExe)
    if ($info.FileDescription.Trim() -ne 'NAX5 Setup') {
        throw "installer FileDescription must be 'NAX5 Setup', got '$($info.FileDescription.Trim())'"
    }

    $dest = Join-Path $env:TEMP ("nax5-inno-smoke-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $dest | Out-Null
    try {
        $setup = Start-Process -FilePath $InstallerExe -ArgumentList @(
            '/VERYSILENT',
            '/SUPPRESSMSGBOXES',
            '/NORESTART',
            "/DIR=`"$dest`""
        ) -Wait -PassThru
        if ($setup.ExitCode -ne 0) {
            throw "Inno installer exited $($setup.ExitCode)"
        }
        Test-ChiakiLaunch -AppDir $dest
        $unins = Get-ChildItem -LiteralPath $dest -Filter 'unins*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($unins) {
            $uninstall = Start-Process -FilePath $unins.FullName -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') -Wait -PassThru
            if ($uninstall.ExitCode -ne 0) {
                Write-Warning "uninstall exited $($uninstall.ExitCode)"
            }
        }
    } finally {
        Stop-ChiakiIfRunning
        if (Test-Path -LiteralPath $dest) {
            Remove-Item -LiteralPath $dest -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    Write-Host "CLEAN_INSTALLER_OK exe=$InstallerExe"
}

# Launched only after the user chooses to restart. Runs outside the install folder.
param(
    [Parameter(Mandatory)][string]$InstallDirectory,
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][string]$StatePath,
    [Parameter(Mandatory)][int]$WaitForProcessId,
    [ValidateSet('0', '1')][string]$StartCore = '1',
    [ValidateRange(1, 60)][int]$StartupTimeoutSeconds = 20
)
$ErrorActionPreference = 'Stop'
$install = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\')
$package = [IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\')
$StatePath = [IO.Path]::GetFullPath($StatePath)
$cache = Split-Path -Parent $StatePath
$parent = Split-Path -Parent $install
$name = Split-Path -Leaf $install
$suffix = [Guid]::NewGuid().ToString('N')
$incoming = Join-Path $parent "$name.update-$suffix"
$backup = Join-Path $parent "$name.previous-$suffix"
$result = Join-Path $cache 'result.json'
$coreStopped = $false
$swapped = $false
$confirmed = $false
$restored = $false
$started = New-Object 'System.Collections.Generic.List[System.Diagnostics.Process]'
# Never the install folder: what starts there would hold it, and the next update could not replace it.
$workingDirectory = [Environment]::GetFolderPath('UserProfile')

function Assert-TemporaryPath([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if ((Split-Path -Parent $absolute) -ne $parent -or
        ((Split-Path -Leaf $absolute) -ne "$name.update-$suffix" -and
         (Split-Path -Leaf $absolute) -ne "$name.previous-$suffix")) { throw '更新临时目录无效。' }
}

# Moves what is in $From into $To (merging folders). A folder some process is in (its current
# directory, e.g. an app started from an older Atrium) cannot be moved itself: its contents go, it stays.
function Move-Tree([string]$From, [string]$To, [switch]$Keep) {
    if (!(Test-Path -LiteralPath $To)) { New-Item -ItemType Directory -Path $To | Out-Null }
    foreach ($item in @(Get-ChildItem -LiteralPath $From -Force)) {
        $target = Join-Path $To $item.Name
        if ($item.PSIsContainer -and (Test-Path -LiteralPath $target)) { Move-Tree $item.FullName $target; continue }
        if ($item.PSIsContainer) {
            try { Move-Item -LiteralPath $item.FullName -Destination $target }
            catch { Move-Tree $item.FullName $target }
            continue
        }
        # A file just written may be open for a moment (a virus scanner reading it).
        for ($try = 1; ; $try++) {
            try { Move-Item -LiteralPath $item.FullName -Destination $target; break }
            catch {
                if ($try -lt 6) { Start-Sleep -Milliseconds 300; continue }
                throw "无法替换 $($item.FullName)（$($_.Exception.Message.TrimEnd('。', '.'))），请关闭正在使用它的程序后重试。"
            }
        }
    }
    if (!$Keep) {
        try { Remove-Item -LiteralPath $From -Force } catch { }
    }
}

function Start-Verified([string]$Executable, [string]$Kind, [string]$Arguments) {
    $eventName = "Local\Atrium.Update.$Kind.$suffix"
    $ready = New-Object System.Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $eventName)
    try {
        $process = Start-Process -FilePath $Executable -ArgumentList "$Arguments --ready-event $eventName" -WorkingDirectory $workingDirectory -WindowStyle Hidden -PassThru
        $started.Add($process)
        $until = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
        do {
            if ($process.HasExited) { throw "$Kind 启动失败（退出码 $($process.ExitCode)）。" }
            if ($ready.WaitOne(100)) {
                if ($process.HasExited) { throw "$Kind 在就绪后退出。" }
                return
            }
        } while ([DateTime]::UtcNow -lt $until)
        throw "$Kind 未确认启动就绪，已保留旧版本。"
    } finally { $ready.Dispose() }
}

function Stop-NewProcesses {
    foreach ($process in $started) {
        if (!$process.HasExited) {
            [void]$process.CloseMainWindow()
            if (!$process.WaitForExit(3000)) { Stop-Process -Id $process.Id -Force }
            [void]$process.WaitForExit(3000)
        }
    }
}

function Write-Result([string]$ErrorText) {
    $temporary = $result + '.tmp'
    @{ error = $ErrorText } | ConvertTo-Json | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $result -Force
}

try {
    if (!$parent -or !$name -or $install -eq [IO.Path]::GetPathRoot($install).TrimEnd('\') -or
        !$package.StartsWith($cache + '\', [StringComparison]::OrdinalIgnoreCase)) { throw '更新目录无效。' }
    Assert-TemporaryPath $incoming
    Assert-TemporaryPath $backup
    foreach ($root in @($install, $package)) {
        if (!(Test-Path -LiteralPath (Join-Path $root 'Atrium.exe')) -or
            !(Test-Path -LiteralPath (Join-Path $root 'Settings\Atrium.Settings.exe'))) { throw '找不到完整的 Atrium 程序文件。' }
    }
    # Copy before stopping the app; disk/access failures leave the old app running.
    New-Item -ItemType Directory -Path $incoming | Out-Null
    Get-ChildItem -LiteralPath $package -Force | Copy-Item -Destination $incoming -Recurse -Force
    # A downloaded release must retain the development package's explicit identity.
    if (Test-Path -LiteralPath (Join-Path $install 'development.txt')) {
        Copy-Item -LiteralPath (Join-Path $install 'development.txt') -Destination $incoming -Force
    }
    $settingsProcess = Get-Process -Id $WaitForProcessId -ErrorAction SilentlyContinue
    if ($settingsProcess -and !$settingsProcess.WaitForExit(30000)) { throw '设置中心尚未退出，更新未安装。' }
    $quit = Start-Process -FilePath (Join-Path $install 'Atrium.exe') -ArgumentList '--exit' -WindowStyle Hidden -PassThru
    try {
        if (!$quit.WaitForExit(15000) -or $quit.ExitCode -ne 0) { throw 'Atrium 尚未正常退出，更新未安装。' }
    } finally { $quit.Dispose() }
    $coreStopped = $true
    Assert-TemporaryPath $backup
    Assert-TemporaryPath $incoming
    # The folder itself stays (an app may be in it); what is in it is replaced.
    try { Move-Tree $install $backup -Keep }
    catch {
        Move-Tree $backup $install
        $restored = $true
        throw
    }
    try { Move-Tree $incoming $install }
    catch {
        Move-Tree $install $incoming -Keep
        Move-Tree $backup $install
        $restored = $true
        throw
    }
    $swapped = $true
    if ($StartCore -eq '1') { Start-Verified (Join-Path $install 'Atrium.exe') 'Core' '' }
    $settingsArguments = if ($StartCore -eq '1') { '--page about --require-core-ready' } else { '--page about' }
    Start-Verified (Join-Path $install 'Settings\Atrium.Settings.exe') 'Settings' $settingsArguments
    foreach ($process in $started) {
        if ($process.HasExited) { throw '新版程序在启动确认期间退出。' }
    }
    # With the core intentionally off, only settings is launched and verified.
    $confirmed = $true
    Remove-Item -LiteralPath $StatePath -Force
    Write-Result ''
} catch {
    $failure = $_.Exception.Message
    if ($swapped -and !$confirmed) {
        try {
            Stop-NewProcesses
            Assert-TemporaryPath $incoming
            Move-Tree $install $incoming -Keep
            Assert-TemporaryPath $backup
            Move-Tree $backup $install
            $restored = $true
        } catch { $failure += " 恢复旧包未完成：$($_.Exception.Message)；恢复副本保留在 $backup。" }
    }
    Write-Result $failure
} finally {
    # Never delete an unconfirmed recovery point, even when rollback itself failed.
    $cleanup = @($incoming)
    if ($confirmed) { $cleanup += $backup }
    foreach ($temporary in $cleanup) {
        Assert-TemporaryPath $temporary
        if (Test-Path -LiteralPath $temporary) {
            try { Remove-Item -LiteralPath $temporary -Recurse -Force } catch { Write-Warning $_.Exception.Message }
        }
    }
    if (!$confirmed -and (!$swapped -or $restored)) {
        if ($coreStopped -and $StartCore -eq '1') {
            Start-Process -FilePath (Join-Path $install 'Atrium.exe') -WorkingDirectory $workingDirectory -WindowStyle Hidden
        }
        Start-Process -FilePath (Join-Path $install 'Settings\Atrium.Settings.exe') -ArgumentList '--page about' -WorkingDirectory $workingDirectory -WindowStyle Hidden
    }
    if ($confirmed) {
        $work = $package
        while ($work.StartsWith($cache + '\', [StringComparison]::OrdinalIgnoreCase) -and (Split-Path -Parent $work) -ne $cache) {
            $work = Split-Path -Parent $work
        }
        $absoluteWork = [IO.Path]::GetFullPath($work)
        if ((Split-Path -Parent $absoluteWork) -eq $cache -and (Split-Path -Leaf $absoluteWork) -match '^package-(background|[0-9a-f]{32})$') {
            try { Remove-Item -LiteralPath $absoluteWork -Recurse -Force } catch { Write-Warning $_.Exception.Message }
        }
    }
    foreach ($process in $started) { $process.Dispose() }
}

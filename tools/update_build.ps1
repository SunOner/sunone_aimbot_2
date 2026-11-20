[CmdletBinding()]
param(
    [ValidateSet('Menu', 'SetBackend', 'Run', 'EnableAtBoot', 'EnableAtLogon', 'Disable', 'InstallPrerequisites')]
    [string]$Action = 'Menu',
    [ValidateSet('CUDA', 'DML')]
    [string]$Backend,
    [switch]$PrepareSystemTools
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$upstream = 'https://github.com/SunOner/sunone_aimbot_2.git'
$logDir = Join-Path $root 'build/updater'

function Invoke-RepoGit {
    & git -C $root @args
    if ($LASTEXITCODE -ne 0) { throw "Git failed ($LASTEXITCODE): $($args -join ' ')" }
}

function Install-MissingTool {
    param([string]$Package, [string[]]$ExtraArgs = @())
    if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) {
        throw "Install $Package or install Windows App Installer and retry."
    }
    & winget.exe install --id $Package --exact --source winget --accept-source-agreements --accept-package-agreements @ExtraArgs
    if ($LASTEXITCODE -ne 0) { throw "Installation of $Package failed ($LASTEXITCODE)." }
}

if ($Action -eq 'InstallPrerequisites') {
    . (Join-Path $PSScriptRoot 'build_common.ps1')
    if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { Install-MissingTool 'Git.Git' }
    try {
        Import-VisualStudioEnvironment
    }
    catch {
        Write-Host $_.Exception.Message
        Install-MissingTool 'Microsoft.VisualStudio.2022.BuildTools' @('--override', '--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
        Import-VisualStudioEnvironment
    }
    $cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
    $versionLine = if ($cmake) { @(& $cmake.Source --version)[0] }
    if ($versionLine -notmatch 'cmake version (\d+\.\d+)' -or [version]$Matches[1] -lt [version]'3.24') {
        Install-MissingTool 'Kitware.CMake'
    }
    # WinGet installers may update PATH outside this PowerShell process.
    $env:PATH = @($env:PATH, [Environment]::GetEnvironmentVariable('Path', 'Machine'),
        [Environment]::GetEnvironmentVariable('Path', 'User')) -join ';'
    if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) { throw 'Reopen the terminal to load the installed Git.' }
    Write-Host 'Windows build tools checked.'
    return
}

# Scope preferences and the task to this user and checkout. Scheduled runs read
# the preference at execution time, so switching backend preserves the trigger.
$rootKey = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash(
    [Text.Encoding]::UTF8.GetBytes($root.ToLowerInvariant()))).Replace('-', '').Substring(0, 16)
$settingsPath = Join-Path $env:LOCALAPPDATA "Sunone/BuildUpdater/$rootKey"
$preference = Join-Path $settingsPath 'backend.txt'

function Save-Backend {
    param([ValidateSet('DML', 'CUDA')][string]$Choice)
    [void][IO.Directory]::CreateDirectory($settingsPath)
    $temp = Join-Path $settingsPath ([Guid]::NewGuid().ToString())
    try {
        [IO.File]::WriteAllText($temp, $Choice.ToUpperInvariant())
        if ([IO.File]::Exists($preference)) {
            [IO.File]::Replace($temp, $preference, "$temp.bak")
        }
        else { [IO.File]::Move($temp, $preference) }
    }
    finally {
        [IO.File]::Delete($temp)
        [IO.File]::Delete("$temp.bak")
    }
}

if ($Action -eq 'SetBackend') {
    if (-not $Backend) { throw 'Specify -Backend DML or -Backend CUDA.' }
    Save-Backend $Backend
    return
}

if (-not $Backend) {
    $Backend = if ([IO.File]::Exists($preference)) { [IO.File]::ReadAllText($preference).Trim() } else { 'DML' }
}

if ($Action -ne 'Run') {
    $user = [Security.Principal.WindowsIdentity]::GetCurrent()
    $taskName = "Sunone build update - $($user.User.Value) - $rootKey"
    if ($Action -eq 'Menu') {
        while ($true) {
            $existing = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
            $schedule = if (-not $existing) { 'off' }
                        elseif ($existing.Triggers[0].CimClass.CimClassName -eq 'MSFT_TaskBootTrigger') { 'at boot' }
                        else { 'at sign-in' }
            Write-Host "Backend: $Backend | Automatic build: $schedule"
            Write-Host '1) Select DML   2) Select CUDA   3) Check and build now'
            Write-Host '4) Enable at boot   5) Enable at sign-in   6) Disable automatic builds'
            Write-Host '7) Install missing Windows build tools   0) Exit'
            try {
                switch (Read-Host 'Choose an option') {
                    '1' { Save-Backend 'DML'; $Backend = 'DML' }
                    '2' { Save-Backend 'CUDA'; $Backend = 'CUDA' }
                    '3' { & $PSCommandPath -Action Run -Backend $Backend -PrepareSystemTools }
                    '4' { & $PSCommandPath -Action EnableAtBoot -Backend $Backend }
                    '5' { & $PSCommandPath -Action EnableAtLogon -Backend $Backend }
                    '6' { & $PSCommandPath -Action Disable }
                    '7' { & $PSCommandPath -Action InstallPrerequisites }
                    '0' { return }
                }
            }
            catch { Write-Warning $_.Exception.Message }
        }
    }
    if ($Action -eq 'Disable') {
        if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
            Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
        }
        return
    }
    Save-Backend $Backend
    $taskArgs = '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -Action Run' -f $PSCommandPath
    $taskAction = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument $taskArgs -WorkingDirectory $root
    $settings = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 5)
    if ($Action -eq 'EnableAtBoot') {
        $trigger = New-ScheduledTaskTrigger -AtStartup
        $password = Read-Host "Windows account password for $($user.Name) (Task Scheduler)" -AsSecureString
        $credential = [PSCredential]::new($user.Name, $password)
        Register-ScheduledTask -TaskName $taskName -Action $taskAction -Trigger $trigger -Settings $settings -RunLevel Limited -User $user.Name -Password $credential.GetNetworkCredential().Password -Force
    }
    else {
        $trigger = New-ScheduledTaskTrigger -AtLogOn -User $user.Name
        $principal = New-ScheduledTaskPrincipal -UserId $user.Name -LogonType Interactive -RunLevel Limited
        Register-ScheduledTask -TaskName $taskName -Action $taskAction -Trigger $trigger -Settings $settings -Principal $principal -Force
    }
    return
}

$build = Join-Path $root ('build/' + $Backend.ToLowerInvariant())
$exe = Join-Path $build 'Release/ai.exe'
$stamp = Join-Path $build 'last-successful-update.txt'

function Get-BuildMode {
    param([string]$Target)
    foreach ($file in @($stamp, $exe, (Join-Path $build 'CMakeCache.txt'), (Join-Path $build 'build-Release.ninja'))) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { return 'Full' }
    }
    $record = [IO.File]::ReadAllLines($stamp)
    if (-not $record.Length) { return 'Full' }
    $previous = $record[0]
    foreach ($dll in ($record | Select-Object -Skip 1)) {
        if (-not (Test-Path -LiteralPath (Join-Path $build "Release/$dll") -PathType Leaf)) { return 'Full' }
    }
    if ($previous -notmatch '^[0-9a-f]{40}$') { return 'Full' }
    if ($previous -eq $Target) { return 'Current' }
    & git -C $root merge-base --is-ancestor $previous $Target
    if ($LASTEXITCODE -ne 0) { return 'Full' }

    $mode = 'Skip'
    # Include deletions and both sides of renames. Unknown inputs take the full path.
    foreach ($path in (Invoke-RepoGit diff --no-renames --name-only $previous $Target --)) {
        if ($path -match '^(docs/|README\.md$|LICENSE$)') { continue }
        if ($path -match '^sunone_aimbot_2/(?!modules/).+\.(c|cc|cpp|cxx|cu|cuh|h|hpp|inl)$') {
            $mode = 'Incremental'
        }
        else { return 'Full' }
    }
    return $mode
}

function Invoke-Wrapper {
    param([string]$Script, [string[]]$Options)
    # Run in a child process so wrapper exits and VS environment changes stay local.
    $shell = if ($PSVersionTable.PSEdition -eq 'Desktop') { Join-Path $PSHOME 'powershell.exe' } else { (Get-Process -Id $PID).Path }
    & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $root $Script) @Options
    if ($LASTEXITCODE -ne 0) { throw "$Script failed ($LASTEXITCODE)." }
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Missing build output: $exe" }
}

[void][IO.Directory]::CreateDirectory($logDir)
# Both backends use the same checkout and dependency directories.
$lock = [IO.File]::Open((Join-Path $logDir 'update.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
try {
    Start-Transcript -Path (Join-Path $logDir "last-$Backend.log") -Force | Out-Null
    try {
        $prepared = $false
        if ($PrepareSystemTools -and -not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
            & $PSCommandPath -Action InstallPrerequisites
            $prepared = $true
        }
        if ((Invoke-RepoGit rev-parse --show-toplevel) -ne ($root -replace '\\','/')) { throw 'Run from a repository checkout.' }
        if ((Invoke-RepoGit branch --show-current) -ne 'main') { throw 'Switch to main or use a dedicated clone.' }
        if (Invoke-RepoGit status --porcelain --untracked-files=no) { throw 'Commit or stash tracked changes before updating.' }

        $oldPrompt = $env:GIT_TERMINAL_PROMPT
        try {
            $env:GIT_TERMINAL_PROMPT = '0'
            Invoke-RepoGit fetch --no-tags $upstream refs/heads/main
        }
        finally { $env:GIT_TERMINAL_PROMPT = $oldPrompt }
        $target = Invoke-RepoGit rev-parse FETCH_HEAD
        & git -C $root merge-base --is-ancestor HEAD $target
        if ($LASTEXITCODE -ne 0) { throw 'Local main cannot fast-forward to upstream main. Resolve the history manually.' }
        Invoke-RepoGit merge --ff-only --no-overwrite-ignore $target
        $mode = Get-BuildMode $target
        Write-Host "$Backend $target - $mode"
        if ($mode -eq 'Current') { Write-Host "Ready: $exe"; return }
        if ($mode -ne 'Skip') {
            if ($PrepareSystemTools -and -not $prepared) { & $PSCommandPath -Action InstallPrerequisites }
            # A partial build may change the output. Retry from full setup next time.
            if (Test-Path -LiteralPath $stamp) { Remove-Item -LiteralPath $stamp }
            if ($mode -eq 'Incremental') {
                try {
                    Invoke-Wrapper 'build_no-options.ps1' @('-Backend', $Backend, '-NonInteractive')
                }
                catch {
                    Write-Warning "$($_.Exception.Message) Retrying with dependency setup."
                    $mode = 'Full'
                }
            }
            if ($mode -eq 'Full') {
                Invoke-Wrapper "tools/build_$($Backend.ToLowerInvariant()).ps1" @('-NonInteractive', '-DownloadOrUpdateNeeded', 'true', '-ExtraCMakeArgs', '--fresh')
            }
        }
        $dlls = @(Get-ChildItem -LiteralPath (Split-Path $exe) -File -Filter '*.dll' | Select-Object -ExpandProperty Name)
        [IO.File]::WriteAllLines("$stamp.tmp", [string[]](@($target) + $dlls))
        Move-Item -LiteralPath "$stamp.tmp" -Destination $stamp -Force
        Write-Host "Ready: $exe"
    }
    finally { Stop-Transcript | Out-Null }
}
finally { $lock.Dispose() }

# Windows host and build prerequisites shared by diagnostics and unattended builds.
. (Join-Path $PSScriptRoot 'build_common.ps1')

function Assert-UpdaterHost {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        throw 'This updater requires Windows 10 or later with x64 PowerShell.'
    }
    $os = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    if ([int]$os.CurrentBuildNumber -lt 10240) { throw 'Use Windows 10 or later.' }
    if (-not [Environment]::Is64BitProcess -or $env:PROCESSOR_ARCHITECTURE -ne 'AMD64') {
        throw 'Open UPDATE_BUILD.bat or use x64 PowerShell. The build wrappers target x64.'
    }
}

function Get-UpdaterCMakeVersion {
    $cmake = Get-Command cmake.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmake) {
        $output = @(& $cmake.Source --version)
        if ($LASTEXITCODE -eq 0 -and $output.Count -gt 0 -and $output[0] -match '^cmake version (\d+\.\d+(?:\.\d+)?)') {
            return [version]$Matches[1]
        }
    }
}

function Assert-UpdaterCompiler {
    $needsSdk = -not ($env:WindowsSdkDir -and $env:WindowsSDKVersion)
    Import-VisualStudioEnvironment -Force:$needsSdk
    if ($env:VSCMD_ARG_TGT_ARCH -and $env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
        throw "MSVC is targeting $env:VSCMD_ARG_TGT_ARCH. Reopen the updater from a fresh x64 shell."
    }
    # VsDevCmd selects the SDK used by cl/link. Check that exact version.
    if (-not $env:WindowsSdkDir -or -not $env:WindowsSDKVersion) {
        throw 'The active Windows SDK is unknown. Use an x64 Native Tools prompt or repair the Windows SDK in Visual Studio Installer.'
    }
    $version = $env:WindowsSDKVersion.TrimEnd('\', '/')
    foreach ($component in "Include/$version/cppwinrt/winrt/base.h", "Include/$version/um/Windows.h",
                            "Lib/$version/um/x64/kernel32.lib", "Lib/$version/ucrt/x64/ucrt.lib") {
        if (-not [IO.File]::Exists((Join-Path $env:WindowsSdkDir $component))) {
            throw "Windows SDK $version is missing $component. Repair that SDK in Visual Studio Installer."
        }
    }
    Write-Host "Windows SDK $version (x64)"
}

function Test-UpdaterEnvironment {
    param([ValidateSet('DML', 'CUDA')][string]$Backend, [switch]$AllowDependencySetup)
    $issues = @()
    $git = Get-Command git.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $git) { $issues += 'Install Git for Windows and reopen the terminal.' }
    try { Assert-UpdaterCompiler } catch { $issues += $_.Exception.Message }
    $cmakeVersion = Get-UpdaterCMakeVersion
    if (-not $cmakeVersion -or $cmakeVersion -lt [version]'3.24') {
        $issues += 'CMake 3.24 or later must be first on PATH. Install it or update the PATH order.'
    }
    else { Write-Host "CMake $cmakeVersion" }

    if ($Backend -eq 'CUDA') {
        if (-not $AllowDependencySetup) {
            $gpu = Get-NvidiaGpuInfo
            if ($gpu) { Write-Host "NVIDIA GPU: $($gpu.Name), driver $($gpu.DriverVersion), compute capability $($gpu.ComputeCapability)" }
            else { Write-Warning 'nvidia-smi could not report a GPU. Runtime GPU compatibility is unverified.' }
        }
        $missing = @()
        $cuda = Get-CudaInstallations | Select-Object -First 1
        if ($cuda) {
            $nvcc = Get-NvccRelease $cuda
            if (-not $nvcc -or $nvcc -lt [version]'13.1') {
                $issues += "CUDA at '$cuda' must provide a working nvcc 13.1 or later. Check CUDA_PATH and installed toolkits."
            }
            else { Write-Host "CUDA $nvcc ($cuda)" }
        }
        else { $missing += 'CUDA Toolkit is missing. Full setup needs the NVIDIA Windows installer.' }
        $trt = Find-TensorRtRoot
        if ($trt) { Write-Host "TensorRT SDK: $trt" }
        else { $missing += 'TensorRT SDK is incomplete or missing. Full setup needs the TensorRT 10 Windows binary archive.' }
        if ($AllowDependencySetup) {
            foreach ($message in $missing) { Write-Warning $message }
        }
        else { $issues += $missing }
    }
    if ($issues.Count) { throw ($issues -join [Environment]::NewLine) }
}

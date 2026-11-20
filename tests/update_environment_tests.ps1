# Standalone tests. Windows probes are replaced with fixtures only in this process.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../tools/update_environment.ps1')

function Expect-Failure {
    param([scriptblock]$Run, [string]$Message)
    try { & $Run }
    catch {
        if ($_.Exception.Message -notmatch $Message) { throw }
        return
    }
    throw "Expected failure matching $Message"
}

function Import-VisualStudioEnvironment { param([switch]$Force) }
function Get-UpdaterCMakeVersion { $script:cmakeVersion }
function Get-CudaInstallations {
    if ($script:cudaForbidden) { throw 'Unexpected CUDA probe' }
    $script:cudaRoot
}
function Get-NvccRelease { $script:nvccVersion }
function Get-NvidiaGpuInfo { if ($script:cudaForbidden) { throw 'Unexpected GPU probe' } }
function Find-TensorRtRoot { $script:tensorRt }
function Get-Command { param($Name, $CommandType, $ErrorAction) if ($Name -eq 'git.exe') { 'git.exe' } }

$fixture = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString())
$savedSdk = $env:WindowsSdkDir
$savedVersion = $env:WindowsSDKVersion
$savedTarget = $env:VSCMD_ARG_TGT_ARCH
try {
    $env:WindowsSdkDir = $fixture
    $env:WindowsSDKVersion = '10.0.22621.0\'
    $env:VSCMD_ARG_TGT_ARCH = 'x64'
    $sdkVersion = '10.0.22621.0'
    foreach ($item in "Include/$sdkVersion/cppwinrt/winrt/base.h", "Include/$sdkVersion/um/Windows.h",
                       "Lib/$sdkVersion/um/x64/kernel32.lib", "Lib/$sdkVersion/ucrt/x64/ucrt.lib") {
        $file = Join-Path $fixture $item
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($file))
        [IO.File]::WriteAllText($file, '')
    }
    $cudaForbidden = $true
    $cmakeVersion = [version]'3.24'
    Test-UpdaterEnvironment DML
    $env:VSCMD_ARG_TGT_ARCH = 'x86'
    Expect-Failure { Test-UpdaterEnvironment DML } 'targeting x86'
    $env:VSCMD_ARG_TGT_ARCH = 'x64'
    $cmakeVersion = [version]'3.23.9'
    Expect-Failure { Test-UpdaterEnvironment DML } 'CMake 3.24'
    $cmakeVersion = [version]'4.0'
    Remove-Item -LiteralPath $file
    Expect-Failure { Test-UpdaterEnvironment DML } 'ucrt.lib'
    [IO.File]::WriteAllText($file, '')

    $cudaForbidden = $false
    $cudaRoot = 'fixture CUDA'
    $nvccVersion = [version]'12.9'
    $tensorRt = 'fixture SDK'
    Expect-Failure { Test-UpdaterEnvironment CUDA } 'nvcc 13.1'
    $nvccVersion = [version]'13.1'
    Test-UpdaterEnvironment CUDA
    $tensorRt = $null
    Expect-Failure { Test-UpdaterEnvironment CUDA } 'TensorRT SDK'
    $cudaRoot = $null
    Expect-Failure { Test-UpdaterEnvironment CUDA } 'CUDA Toolkit is missing'
    Test-UpdaterEnvironment CUDA -AllowDependencySetup
}
finally {
    $env:WindowsSdkDir = $savedSdk
    $env:WindowsSDKVersion = $savedVersion
    $env:VSCMD_ARG_TGT_ARCH = $savedTarget
    if (Test-Path $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
}
Write-Host 'Windows compatibility fixture checks passed.'

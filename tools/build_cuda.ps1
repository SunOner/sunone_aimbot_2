[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$RepoRoot = "",
    [string]$BuildDir = "build\cuda",
    [ValidateSet("Release", "RelWithDebInfo", "MinSizeRel", "Debug")]
    [string]$Configuration = "Release",
    [string]$Generator = "Ninja Multi-Config",
    [object]$OpenCvAlreadyBuilt = $null,
    [object]$DownloadOrUpdateNeeded = $null,
    [switch]$UseLatestPackages,
    [switch]$OpenBrowserForDownloads,
    [switch]$SkipOpenCvBuild,
    [string]$CudaArchBin = "",
    [string]$OpenCvBuildList = "",
    [ValidateRange(0, 256)]
    [int]$OpenCvMaxCpuCount = 0,
    [switch]$OpenCvCleanBuild,
    [switch]$NonInteractive,
    [switch]$DryRun,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraCMakeArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. "$PSScriptRoot\build_common.ps1"

if (-not [string]::IsNullOrWhiteSpace($RepoRoot)) {
    $script:RepoRootOverride = $RepoRoot
}

$pushedLocation = $false
try {
    $repo = Get-RepoRoot
    Push-Location $repo
    $pushedLocation = $true

    $allowDownloads = Resolve-OptionalBoolean -Value $DownloadOrUpdateNeeded -Question "Download or update needed files?" -Default $true -NonInteractive:$NonInteractive

    Import-VisualStudioEnvironment
    $ninja = Ensure-Ninja -AllowDownload:$allowDownloads -DryRun:$DryRun
    Ensure-CoreSourceModules -AllowDownload:$allowDownloads -DryRun:$DryRun

    $resolution = Get-BestCompatibleCudaDependencySet
    if (-not [string]::IsNullOrWhiteSpace($CudaArchBin)) {
        $resolution.CudaArchBin = $CudaArchBin
    }
    $effectiveOpenCvBuildList = $OpenCvBuildList
    if (-not [string]::IsNullOrWhiteSpace($effectiveOpenCvBuildList)) {
        $opencvBuildModules = @($effectiveOpenCvBuildList -split "," | ForEach-Object { $_.Trim() } | Where-Object { $_ })
        $requiredOpenCvModules = @("cudev", "core", "imgproc", "imgcodecs", "videoio", "highgui")
        foreach ($module in $requiredOpenCvModules) {
            if ($opencvBuildModules -notcontains $module) {
                $opencvBuildModules = @($module) + $opencvBuildModules
            }
        }
        $effectiveOpenCvBuildList = ($opencvBuildModules | Select-Object -Unique) -join ","
    }

    $downloadItems = @()
    if (-not $resolution.CudaRoot) {
        $downloadItems += New-DependencyDownloadItem `
            -Id "cuda" `
            -Name "CUDA Toolkit" `
            -Version $resolution.DesiredCudaVersion `
            -Reason "CUDA build requires nvcc.exe and CUDA runtime libraries." `
            -DownloadUrl "https://developer.nvidia.com/cuda-downloads" `
            -ExpectedFilePatterns @("cuda_*_windows*.exe", "cuda_*_win*.exe") `
            -Destination "$env:ProgramFiles\NVIDIA GPU Computing Toolkit\CUDA" `
            -Action "install"
    }
    if (-not $resolution.TensorRtRoot) {
        $downloadItems += New-DependencyDownloadItem `
            -Id "tensorrt" `
            -Name "TensorRT" `
            -Version $resolution.DesiredTensorRtVersion `
            -Reason "CUDA build requires TensorRT headers, import libraries, and runtime DLLs." `
            -DownloadUrl "https://developer.nvidia.com/tensorrt/download" `
            -ExpectedFilePatterns @("TensorRT-10*.Windows*.zip", "TensorRT-10*.win10*.zip") `
            -Destination (Resolve-RepoPath "sunone_aimbot_2\modules") `
            -Action "extract"
    }

    if ($downloadItems.Count -gt 0) {
        if (-not $allowDownloads) {
            $manifest = Write-DependencyDownloadManifest -Items $downloadItems
            throw "Required CUDA dependencies are missing. Download list written to $($manifest.MarkdownPath). Re-run with downloads enabled after downloading."
        }

        $copied = @(Invoke-GuidedDependencyDownloads -Items $downloadItems -OpenBrowser:$OpenBrowserForDownloads -NonInteractive:$NonInteractive -DryRun:$DryRun)
        foreach ($item in $copied) {
            if ($item.action -eq "install") {
                Expand-DependencyArchive -ArchivePath $item.cached -Destination $item.destination
            }
            elseif ($item.action -eq "extract") {
                Expand-DependencyArchive -ArchivePath $item.cached -Destination $item.destination
            }
        }
        $resolution = Get-BestCompatibleCudaDependencySet
        if (-not $resolution.TensorRtRoot -and ($copied | Where-Object { $_.id -eq "tensorrt" })) {
            $trtFile = ($copied | Where-Object { $_.id -eq "tensorrt" } | Select-Object -First 1).cached
            throw "TensorRT archive was extracted but a valid Windows SDK layout was not found. Expected include\NvInfer.h, lib\nvinfer_10.lib, lib\nvonnxparser_10.lib, and bin\nvinfer_10.dll under sunone_aimbot_2\modules\TensorRT-*. The file may be source code instead of the Windows binary SDK: $trtFile"
        }
    }

    if (-not $resolution.CudaRoot) {
        if ($DryRun) {
            $resolution.CudaRoot = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.x"
            $resolution.CudaVersion = "13.x"
        }
        else {
            throw "CUDA Toolkit was not detected after dependency setup."
        }
    }
    if (-not $resolution.TensorRtRoot) {
        if ($DryRun) {
            $resolution.TensorRtRoot = Resolve-RepoPath "sunone_aimbot_2\modules\TensorRT-10.x"
        }
        else {
            throw "TensorRT was not detected after dependency setup."
        }
    }

    $opencvCudaInstall = Resolve-RepoPath "sunone_aimbot_2\modules\opencv\build\cuda\install"
    $opencvLayout = Get-OpenCvWorldLayout -Root $opencvCudaInstall -Configuration $Configuration
    $opencvBuilt = Resolve-OpenCvAlreadyBuilt -Value $OpenCvAlreadyBuilt -Backend "cuda" -Layout $opencvLayout -NonInteractive:$NonInteractive
    if (-not $opencvBuilt -or -not $opencvLayout) {
        if ($SkipOpenCvBuild) {
            throw "CUDA OpenCV layout is missing or invalid: $opencvCudaInstall"
        }
        if (-not $allowDownloads) {
            throw "CUDA OpenCV is not built. Re-run with download/update enabled so sources can be cloned and built."
        }

        $opencvGenerator = if ($Generator -eq "Ninja Multi-Config") { "Ninja" } else { $Generator }
        $opencvArgs = @(
            "-Generator", $opencvGenerator,
            "-NinjaPath", $ninja,
            "-Configuration", $Configuration,
            "-CudaPath", $resolution.CudaRoot,
            "-CudaArchBin", $resolution.CudaArchBin,
            "-DisableCuDNN"
        )
        if (-not [string]::IsNullOrWhiteSpace($effectiveOpenCvBuildList)) {
            $opencvArgs += @("-BuildList", $effectiveOpenCvBuildList)
        }
        if ($OpenCvMaxCpuCount -gt 0) {
            $opencvArgs += @("-MaxCpuCount", $OpenCvMaxCpuCount.ToString())
        }
        if ($OpenCvCleanBuild) {
            $opencvArgs += "-CleanBuild"
        }
        if ($DryRun) { $opencvArgs += "-DryRun" }
        $psArgs = @(
            "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", (Resolve-RepoPath "tools\build_opencv_cuda.ps1")
        ) + $opencvArgs
        Invoke-External "powershell" $psArgs -DryRun:$DryRun
    }

    $buildPath = Resolve-RepoPath $BuildDir
    $resolutionPath = Write-DependencyResolution -Resolution ([pscustomobject]@{
        backend = "cuda"
        compatibilityProfile = $resolution.CompatibilityProfile
        desiredCudaVersion = $resolution.DesiredCudaVersion
        desiredTensorRtVersion = $resolution.DesiredTensorRtVersion
        desiredOpenCvVersion = $resolution.DesiredOpenCvVersion
        generator = $Generator
        configuration = $Configuration
        ninja = $ninja
        cudaRoot = $resolution.CudaRoot
        cudaVersion = $resolution.CudaVersion
        cudaArchBin = $resolution.CudaArchBin
        tensorRtRoot = $resolution.TensorRtRoot
        cudnnRoot = $resolution.CudnnRoot
        useCudnnForOpenCvDnn = $false
        opencvRoot = $opencvCudaInstall
    })
    Write-BuildStep "Dependency resolution written to $resolutionPath" "cuda"

    $cmakeArgs = @(
        "-S", (ConvertTo-CMakePath $repo),
        "-B", (ConvertTo-CMakePath $buildPath),
        "-G", $Generator,
        "-DCMAKE_MAKE_PROGRAM=$(ConvertTo-CMakePath $ninja)",
        "-DAIMBOT_USE_CUDA=ON",
        "-DAIMBOT_TENSORRT_ROOT=$(ConvertTo-CMakePath $resolution.TensorRtRoot)",
        "-DCMAKE_CUDA_FLAGS=--allow-unsupported-compiler",
        "-DCUDA_NVCC_FLAGS=--allow-unsupported-compiler"
    )
    if ($resolution.CudnnRoot) {
        $cmakeArgs += "-DAIMBOT_CUDNN_ROOT=$(ConvertTo-CMakePath $resolution.CudnnRoot)"
    }
    if ($ExtraCMakeArgs) {
        $cmakeArgs += $ExtraCMakeArgs
    }

    Write-BuildStep "Configuring $BuildDir with $Generator" "cuda"
    Invoke-External "cmake" $cmakeArgs -DryRun:$DryRun

    Write-BuildStep "Building $Configuration" "cuda"
    Invoke-External "cmake" @(
        "--build", (ConvertTo-CMakePath $buildPath),
        "--config", $Configuration,
        "--parallel"
    ) -DryRun:$DryRun

    Write-BuildStep "Done: $BuildDir\$Configuration\ai.exe" "cuda"
}
finally {
    if ($pushedLocation) {
        Pop-Location
    }
}

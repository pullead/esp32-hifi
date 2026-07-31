param(
    [string]$Environment = "esp32s3_OTA",
    [string]$BuildPath = "C:\mwr-src",
    [string]$BuildDrive = "P:",
    [int]$Jobs = 1,
    [switch]$Clean,
    [switch]$UseSubst,
    [switch]$CheckOnly,
    [switch]$FullBuild
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$pio = Get-Command pio -ErrorAction SilentlyContinue
if (-not $pio) {
    $candidate = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
    if (Test-Path $candidate) {
        $pio = Get-Item $candidate
    } else {
        throw "PlatformIO was not found. Install it or add pio.exe to PATH."
    }
}

$drive = $BuildDrive.TrimEnd("\")
if ($drive.Length -eq 1) { $drive = "${drive}:" }
$createdSubst = $false
$workDir = $repoRoot

function Same-ResolvedPath([string]$Left, [string]$Right) {
    return (Resolve-Path $Left).Path.TrimEnd("\") -ieq (Resolve-Path $Right).Path.TrimEnd("\")
}

if ($repoRoot -match "\s") {
    if ($UseSubst) {
        $existing = (& cmd /c subst $drive 2>$null) -join "`n"
        if ($LASTEXITCODE -eq 0 -and $existing) {
            $mappedPath = ($existing -replace "^[A-Z]:\\: => ", "").Trim()
            if (-not (Same-ResolvedPath $mappedPath $repoRoot)) {
                throw "$drive is already mapped to '$mappedPath'. Re-run with -BuildDrive using a free drive letter."
            }
        } else {
            & cmd /c subst $drive $repoRoot
            if ($LASTEXITCODE -ne 0) { throw "Failed to create subst drive $drive for '$repoRoot'." }
            $createdSubst = $true
        }
        $workDir = "$drive\"
    } else {
        if (Test-Path $BuildPath) {
            $item = Get-Item $BuildPath
            $target = $item.Target
            if ($target -is [array]) { $target = $target[0] }
            if ($target) {
                if (-not (Same-ResolvedPath $target $repoRoot)) {
                    throw "$BuildPath already points to '$target'. Re-run with -BuildPath using another no-space path."
                }
            } elseif (-not (Same-ResolvedPath $BuildPath $repoRoot)) {
                throw "$BuildPath already exists and is not this repository. Re-run with -BuildPath using another no-space path."
            }
        } else {
            New-Item -ItemType Junction -Path $BuildPath -Target $repoRoot | Out-Null
        }
        $workDir = $BuildPath
    }
}

Write-Host "Repository: $repoRoot"
Write-Host "Build workdir: $workDir"
Write-Host "PlatformIO: $($pio.FullName)"
Write-Host "Environment: $Environment"
Write-Host "Jobs: $Jobs"
Write-Host "Target: $(if ($FullBuild) { 'default full build' } else { 'buildprog' })"

if ($CheckOnly) {
    exit 0
}

try {
    Push-Location $workDir
    if ($Clean) {
        & $pio.FullName run -e $Environment -t clean
        if ($LASTEXITCODE -ne 0) { throw "PlatformIO clean failed." }
    }
    if ($FullBuild) {
        & $pio.FullName run -e $Environment -j $Jobs
    } else {
        & $pio.FullName run -e $Environment -t buildprog -j $Jobs
    }
    if ($LASTEXITCODE -ne 0) { throw "PlatformIO build failed." }
} finally {
    Pop-Location
    if ($createdSubst) {
        & cmd /c subst $drive /D
    }
}

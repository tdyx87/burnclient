# WSL2 build launcher for BurningDLL
# Usage:
#   .\build_wsl2.ps1 [-BuildType Debug|Release|RelWithDebInfo|MinSizeRel] [-Clean] [-Distro Ubuntu]
param(
    [ValidateSet("Debug","Release","RelWithDebInfo","MinSizeRel")]
    [string]$BuildType = "Release",
    [switch]$Clean,
    [string]$Distro = "Ubuntu"
)

$ErrorActionPreference = "Stop"

$ProjectDir = (Resolve-Path "$PSScriptRoot").Path
$ProjectDirWsl = ($ProjectDir -replace '\\','/') -replace '^([A-Za-z]):','/mnt/$1'.ToLower()
$ScriptWsl = "$ProjectDirWsl/build_wsl2.sh"

Write-Host "============================================"
Write-Host " BurningDLL - WSL2 build launcher"
Write-Host " Project (Windows): $ProjectDir"
Write-Host " Project (WSL):     $ProjectDirWsl"
Write-Host " Build type:        $BuildType"
Write-Host " Distro:            $Distro"
Write-Host "============================================"

# Ensure WSL is available
if (-not (Get-Command wsl -ErrorAction SilentlyContinue)) {
    Write-Error "wsl.exe not found. Please install WSL2 first: wsl --install"
    exit 1
}

# Check the distro is installed
$distros = (wsl -l -q 2>$null) -split "`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ }
if ($distros -notcontains $Distro) {
    Write-Host "Available distros: $($distros -join ', ')"
    Write-Error "Distro '$Distro' not found. Install it with: wsl --install -d $Distro"
    exit 1
}

# Make sure the script has unix line endings and is executable
$scriptPath = Join-Path $ProjectDir "build_wsl2.sh"
if (-not (Test-Path $scriptPath)) {
    Write-Error "build_wsl2.sh not found in $ProjectDir"
    exit 1
}

# Convert CRLF -> LF in case it was edited on Windows
$bytes = [System.IO.File]::ReadAllBytes($scriptPath)
$text = [System.Text.Encoding]::UTF8.GetString($bytes)
$text = $text -replace "`r`n","`n"
[System.IO.File]::WriteAllBytes($scriptPath, [System.Text.Encoding]::UTF8.GetBytes($text))

# Run build inside WSL2
$cleanArg = if ($Clean) { "clean" } else { "" }
$args = @("-d", $Distro, "bash", $ScriptWsl, $BuildType)
if ($cleanArg) { $args += $cleanArg }

Write-Host ""
Write-Host "[WSL] Running: wsl $($args -join ' ')"
& wsl @args
$exit = $LASTEXITCODE

if ($exit -ne 0) {
    Write-Error "WSL build failed with exit code $exit"
    exit $exit
}

Write-Host ""
Write-Host "Build succeeded. Outputs under: $ProjectDir\build_linux\"
Write-Host "  Library: build_linux\lib\libburning_dll.so"
Write-Host "  Tests:   build_linux\bin\test_burning_utility"

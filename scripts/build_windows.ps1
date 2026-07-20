#Requires -Version 5.1

param(
    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $ProjectRoot "build"

# Try to locate cmake from Visual Studio if not in PATH
$CMakeExe = Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (-not $CMakeExe) {
    $VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $VsWhere) {
        $VsPath = & $VsWhere -latest -products * -property installationPath
        if ($VsPath) {
            $CMakeExe = Join-Path $VsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        }
    }
}
if (-not $CMakeExe -or -not (Test-Path $CMakeExe)) {
    throw "CMake not found. Please install 'C++ CMake tools for Windows' via Visual Studio Installer."
}

Write-Host "[Phase 0] Using CMake: $CMakeExe"
Write-Host "[Phase 0] Project root: $ProjectRoot"

if ($Rebuild -and (Test-Path $BuildDir)) {
    Write-Host "[Phase 0] Removing existing build directory..."
    Remove-Item -Recurse -Force $BuildDir
}

if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

Set-Location $BuildDir

Write-Host "[Phase 0] Configuring with CMake..."
& $CMakeExe .. -G "Visual Studio 17 2022" -A x64

if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed"
}

Write-Host "[Phase 0] Building..."
& $CMakeExe --build . --config Release

if ($LASTEXITCODE -ne 0) {
    throw "Build failed"
}

Write-Host "[Phase 0] Running tests..."
& $CMakeExe --build . --config Release --target RUN_TESTS

if ($LASTEXITCODE -ne 0) {
    throw "Tests failed"
}

Write-Host "[Phase 0] Done. Run .\bin\api_server.exe"

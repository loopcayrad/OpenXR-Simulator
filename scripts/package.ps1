<#
.SYNOPSIS
    Bundle the OpenXR Simulator Release build into a redistributable zip.

.DESCRIPTION
    Collects the self-contained runtime + tray manager into one folder using a
    relocatable layout (the manifest points at .\openxr_simulator.dll so the zip
    works from any unzip location), then compresses it to a versioned zip ready
    for a GitHub release. Nothing here depends on Vulkan being installed on the
    target machine -- only the headers were needed at build time.

.PARAMETER BuildDir
    CMake build tree. Defaults to ./build.

.PARAMETER Config
    CMake build configuration to package. Defaults to Release.

.PARAMETER OutDir
    Where to drop the zip. Defaults to ./dist.
#>
[CmdletBinding()]
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..' 'build'),
    [string]$Config   = 'Release',
    [string]$OutDir   = (Join-Path $PSScriptRoot '..' 'dist')
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = (Resolve-Path $BuildDir -ErrorAction SilentlyContinue).Path
if (-not $build) { throw "Build tree not found at $BuildDir. Build first." }

# --- locate artifacts (CMake multi-config puts them under build/<Config>/bin) ---
function Find-Artifact {
    param([string]$Name)
    $candidates = @(
        (Join-Path $build 'bin' $Name)
        (Join-Path $build $Config 'bin' $Name)
        (Join-Path $build $Name)
        (Join-Path $root 'bin' $Name)
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { return (Resolve-Path $c).Path }
    }
    throw "Artifact not found: $Name (looked in build/bin, build/$Config/bin, bin/)."
}

$dll         = Find-Artifact 'openxr_simulator.dll'
$managerExe  = Find-Artifact 'openxr_simulator_manager.exe'
$activatePs1 = (Resolve-Path (Join-Path $root 'activate_simulator.ps1')).Path
$deactivatePs1 = (Resolve-Path (Join-Path $root 'deactivate_simulator.ps1')).Path

# --- assemble package folder ---
$version = & {
    $v = (Get-Content (Join-Path $root 'CMakeLists.txt') | Where-Object { $_ -match 'project\(OpenXR-Simulator VERSION ([0-9.]+)' })
    if ($v -match 'VERSION ([0-9.]+)') { return $Matches[1] }
    return '0.0.0'
}
$pkgName = "OpenXR-Simulator-v$version"
$pkgRoot = Join-Path $OutDir $pkgName
if (Test-Path $pkgRoot) { Remove-Item -Recurse -Force $pkgRoot }
New-Item -ItemType Directory -Force $pkgRoot | Out-Null

Copy-Item -LiteralPath $dll        -Destination (Join-Path $pkgRoot 'openxr_simulator.dll')
Copy-Item -LiteralPath $managerExe -Destination (Join-Path $pkgRoot 'openxr_simulator_manager.exe')
Copy-Item -LiteralPath $activatePs1   -Destination (Join-Path $pkgRoot 'activate_simulator.ps1')
Copy-Item -LiteralPath $deactivatePs1 -Destination (Join-Path $pkgRoot 'deactivate_simulator.ps1')

# Relocatable manifest: library_path resolves next to the dll wherever unzipped.
$manifest = @{
    file_format_version = '1.0.0'
    runtime             = @{ library_path = '.\openxr_simulator.dll'; name = 'OpenXR Simulator' }
} | ConvertTo-Json -Depth 3
Set-Content -Path (Join-Path $pkgRoot 'openxr_simulator.json') -Value $manifest -Encoding ascii

# --- compress ---
$zip = Join-Path $OutDir "$pkgName.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $pkgRoot '*') -DestinationPath $zip -Force

Write-Host "Packaged $zip" -ForegroundColor Green
Write-Host "Contents:" -ForegroundColor Cyan
Get-ChildItem $pkgRoot | ForEach-Object { Write-Host "  $($_.Name)" }

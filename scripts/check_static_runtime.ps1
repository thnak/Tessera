# SPDX-License-Identifier: MIT
# Thin Windows wrapper for scripts/check_static_runtime.py. It translates PowerShell-style
# parameters to the Python CLI and keeps the script's exit codes: 0 pass/skip, 1 static-link
# violation, 2 tool/setup failure.
param(
    [string]$Config,
    [string]$Target,
    [string]$BuildDir,
    [string]$Artifact,
    [switch]$SkipBuild,
    [string]$FromOutput,
    [string]$Os,
    [string]$Tool,
    [string]$Dumpbin,
    [string]$Allowlist,
    [string]$Cmake,
    [string]$Generator
)

$options = @()
if ($Config) { $options += @('--config', $Config) }
if ($Target) { $options += @('--target', $Target) }
if ($BuildDir) { $options += @('--build-dir', $BuildDir) }
if ($Artifact) { $options += @('--artifact', $Artifact) }
if ($SkipBuild) { $options += '--skip-build' }
if ($FromOutput) { $options += @('--from-output', $FromOutput) }
if ($Os) { $options += @('--os', $Os) }
if ($Tool) { $options += @('--tool', $Tool) }
if ($Dumpbin) { $options += @('--dumpbin', $Dumpbin) }
if ($Allowlist) { $options += @('--allowlist', $Allowlist) }
if ($Cmake) { $options += @('--cmake', $Cmake) }
if ($Generator) { $options += @('--generator', $Generator) }

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    Write-Error 'python not found on PATH'
    exit 2
}

$script = Join-Path $PSScriptRoot 'check_static_runtime.py'
& $python.Source -B $script @options
exit $LASTEXITCODE

# SPDX-License-Identifier: MIT
# Thin Windows wrapper for scripts/check_reproducible.py. Keeps the exit codes of the Python core
# (0 identical, 1 differs, 2 setup/tool failure) and forwards the usual options.
param(
    [string]$Config,
    [string]$Target,
    [string]$BuildRoot,
    [string]$Cmake,
    [string]$Generator,
    [string[]]$Compare,
    [switch]$Keep,
    [switch]$Json
)

$options = @()
if ($Config) { $options += @('--config', $Config) }
if ($Target) { $options += @('--target', $Target) }
if ($BuildRoot) { $options += @('--build-root', $BuildRoot) }
if ($Cmake) { $options += @('--cmake', $Cmake) }
if ($Generator) { $options += @('--generator', $Generator) }
if ($Compare) { $options += @('--compare', $Compare[0], $Compare[1]) }
if ($Keep) { $options += '--keep' }
if ($Json) { $options += '--json' }

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    Write-Error 'python not found on PATH'
    exit 2
}

$script = Join-Path $PSScriptRoot 'check_reproducible.py'
& $python.Source -B $script @options
exit $LASTEXITCODE

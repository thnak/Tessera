# SPDX-License-Identifier: MIT
# Thin Windows wrapper for scripts/gates.py; forwards options and keeps its exit codes
# (0 all gates passed, 1 a gate failed, 2 setup failure).
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    Write-Error 'python not found on PATH'
    exit 2
}

$script = Join-Path $PSScriptRoot 'gates.py'
& $python.Source -B $script @Rest
exit $LASTEXITCODE

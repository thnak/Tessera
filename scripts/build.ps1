# SPDX-License-Identifier: MIT
# Configures and builds with a Ninja preset from any terminal: it loads the MSVC environment
# itself, so no Developer shell or Visual Studio IDE is needed. Ninja parallelizes per file and
# uses sccache/ccache when installed (see cmake/CompilerCache.cmake).
#
#   .\scripts\build.ps1                      # release build
#   .\scripts\build.ps1 -Preset dev -Test    # debug build, then run ctest
#   .\scripts\build.ps1 -Target scene_graph_tests
param(
    [ValidateSet('dev', 'release', 'ci')][string]$Preset = 'release',
    [string]$Target,
    [switch]$Test,
    [switch]$Reconfigure
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Enter-VsDevShell {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload.'
    }
    $install = & $vswhere -latest -prerelease -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $install) {
        throw 'No Visual Studio install with the C++ x64 tools was found.'
    }

    $vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
    # Run vcvars in cmd, dump the resulting environment, and copy it into this process.
    $lines = cmd /c "`"$vcvars`" >nul && set"
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
        }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "cl.exe is still not on PATH after loading $vcvars."
    }
}

function Invoke-Native {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Exe $($Arguments -join ' ') failed with exit code $LASTEXITCODE"
    }
}

Enter-VsDevShell
Push-Location $root
try {
    $buildDir = Join-Path $root "build\$Preset"
    if ($Reconfigure -or -not (Test-Path (Join-Path $buildDir 'build.ninja'))) {
        Invoke-Native cmake @('--preset', $Preset)
    }

    $buildArgs = @('--build', '--preset', $Preset)
    if ($Target) { $buildArgs += @('--target', $Target) }
    $elapsed = Measure-Command { Invoke-Native cmake $buildArgs }
    Write-Host ('Build finished in {0:mm\:ss}' -f $elapsed)

    if ($Test) {
        Invoke-Native ctest @('--preset', $Preset)
    }
}
finally {
    Pop-Location
}

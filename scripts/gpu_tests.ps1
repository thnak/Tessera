# SPDX-License-Identifier: MIT
# Builds Tessera in Release with Ninja and runs the GPU/window tests on the machine's real GPU.
# These tests skip on hosted CI (no window backend, no presentation surface), so they only prove
# anything where a display and a Vulkan device exist. This lane
# is deliberately not a GitHub workflow: the machine is not always online and a self-hosted runner
# on a public repository would run outside code. Run it by hand: docs/guides/real-gpu-ci.md.
#
#   .\scripts\gpu_tests.ps1                       # build, then run every GPU/window test once
#   .\scripts\gpu_tests.ps1 -Repeat 5             # run each test up to 5 times, stop on a failure
#   .\scripts\gpu_tests.ps1 -Filter caret_blink   # only the tests matching this ctest regex
param(
    [string]$Filter = 'app_window|vulkan_|caret_blink|app_input|gpu',
    [int]$Repeat = 1,
    [string]$BuildDir = 'build\gpu-real',
    [switch]$Reconfigure
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Enter-VsDevShell {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $install = $null
    if (Test-Path $vswhere) {
        $install = & $vswhere -latest -prerelease -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
    if (-not $install) {
        $candidates = Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\*\*" -Directory `
            -ErrorAction SilentlyContinue | Where-Object {
                Test-Path (Join-Path $_.FullName 'VC\Auxiliary\Build\vcvars64.bat') }
        $install = ($candidates | Select-Object -Last 1).FullName
    }
    if (-not $install) { throw 'No Visual Studio / Build Tools install with the C++ x64 tools.' }
    $vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
}

Enter-VsDevShell

Write-Host '--- GPU devices ---'
if (Get-Command vulkaninfo -ErrorAction SilentlyContinue) {
    vulkaninfo --summary 2>&1 | Out-String -Stream | Select-String -Pattern 'deviceName|deviceType|apiVersion|driverName' |
        ForEach-Object { $_.Line.Trim() }
} else {
    Write-Host 'vulkaninfo not on PATH (the Vulkan SDK provides it); continuing.'
}

if ($Reconfigure -or -not (Test-Path (Join-Path $BuildDir 'build.ninja'))) {
    cmake -S . -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release `
        -DTESSERA_BUILD_TESTS=ON -DTESSERA_WARNINGS_AS_ERRORS=OFF
    if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
}
cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

Write-Host "--- ctest -R '$Filter' (repeat $Repeat) ---"
$ctestArgs = @('--test-dir', $BuildDir, '-R', $Filter, '--output-on-failure', '--timeout', '300')
if ($Repeat -gt 1) { $ctestArgs += @('--repeat', "until-fail:$Repeat") }
ctest @ctestArgs
$code = $LASTEXITCODE

# A test that printed "skipped" proves nothing on this lane: surface it so a runner with no
# display or driver does not look green.
$log = Join-Path $BuildDir 'Testing\Temporary\LastTest.log'
if (Test-Path $log) {
    $skipped = Select-String -Path $log -Pattern 'skipped \(' | ForEach-Object { $_.Line.Trim() } |
        Sort-Object -Unique
    if ($skipped) {
        Write-Host '--- tests that skipped (no window backend or surface on this machine) ---'
        $skipped | ForEach-Object { Write-Host $_ }
        if ($env:TESSERA_GPU_FAIL_ON_SKIP -eq '1') { $code = 1 }
    }
}
exit $code

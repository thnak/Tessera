# SPDX-License-Identifier: MIT
param([string]$BuildDir = 'build')

$root = Split-Path -Parent $PSScriptRoot

$clangTidy = $null
$cmd = Get-Command clang-tidy -ErrorAction SilentlyContinue
if ($cmd) {
    $clangTidy = $cmd.Source
}
if (-not $clangTidy) {
    $candidates = @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\clang-tidy.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-tidy.exe',
        'C:\Program Files\LLVM\bin\clang-tidy.exe'
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            $clangTidy = $candidate
            break
        }
    }
}

if (-not $clangTidy) {
    Write-Error 'clang-tidy not found. Install LLVM or set PATH.'
    exit 1
}

$compileCommands = Join-Path (Join-Path $root $BuildDir) 'compile_commands.json'
if (-not (Test-Path $compileCommands)) {
    Write-Error "compile_commands.json not found. Configure with the Ninja preset (CMAKE_EXPORT_COMPILE_COMMANDS=ON)."
    exit 1
}

$extensions = @('*.cpp', '*.cc', '*.cxx')
$files = Get-ChildItem -Path $root -Recurse -File -Include $extensions |
    Where-Object {
        $_.FullName -notmatch '\\build(\\|$)' -and
        $_.FullName -notmatch '\\third_party\\stb\\' -and
        $_.FullName -notmatch '\\\.git\\'
    }

$failed = 0
foreach ($file in $files) {
    & $clangTidy -p (Join-Path $root $BuildDir) --quiet $file.FullName
    if ($LASTEXITCODE -ne 0) {
        $failed++
    }
}

if ($failed -gt 0) {
    Write-Error "$failed file(s) reported clang-tidy issues."
    exit 1
}

Write-Output "clang-tidy checked $($files.Count) file(s)."

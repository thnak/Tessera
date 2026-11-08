# SPDX-License-Identifier: MIT
param([switch]$Check)

$root = Split-Path -Parent $PSScriptRoot

$clangFormat = $null
$cmd = Get-Command clang-format -ErrorAction SilentlyContinue
if ($cmd) {
    $clangFormat = $cmd.Source
}
if (-not $clangFormat) {
    $candidates = @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\clang-format.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-format.exe',
        'C:\Program Files\LLVM\bin\clang-format.exe'
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            $clangFormat = $candidate
            break
        }
    }
}

if (-not $clangFormat) {
    Write-Error 'clang-format not found. Install LLVM or set PATH.'
    exit 1
}

$extensions = @('*.cpp', '*.hpp', '*.h', '*.cc', '*.cxx')
$files = Get-ChildItem -Path $root -Recurse -File -Include $extensions |
    Where-Object {
        $_.FullName -notmatch '\\build(\\|$)' -and
        $_.FullName -notmatch '\\third_party\\(stb|volk|vma|yoga)\\' -and
        $_.FullName -notmatch '\\\.git\\'
    }

$failed = 0
foreach ($file in $files) {
    if ($Check) {
        & $clangFormat --dry-run --Werror --style=file $file.FullName
    } else {
        & $clangFormat -i --style=file $file.FullName
    }
    if ($LASTEXITCODE -ne 0) {
        $failed++
    }
}

if ($failed -gt 0) {
    Write-Error "$failed file(s) failed formatting."
    exit 1
}

Write-Output "Formatted/checked $($files.Count) file(s)."

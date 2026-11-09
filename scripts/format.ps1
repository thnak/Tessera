# SPDX-License-Identifier: MIT
param([switch]$Check)

$root = Split-Path -Parent $PSScriptRoot

$clangFormat = $env:CLANG_FORMAT
$cmd = $null
if (-not $clangFormat) {
    $cmd = Get-Command clang-format -ErrorAction SilentlyContinue
}
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

# Warn (but do not fail) when the formatter differs from the pinned version; the format-version gate
# in scripts/gates.py is the enforcement point.
$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) {
    $previousClangFormat = $env:CLANG_FORMAT
    $env:CLANG_FORMAT = $clangFormat
    & $python.Source -B (Join-Path $PSScriptRoot 'check_format_version.py') *> $null
    $versionStatus = $LASTEXITCODE
    if ($previousClangFormat) {
        $env:CLANG_FORMAT = $previousClangFormat
    } else {
        Remove-Item Env:CLANG_FORMAT -ErrorAction SilentlyContinue
    }
    if ($versionStatus -ne 0) {
        Write-Warning 'clang-format does not match tools/format-version.txt; run: python -B scripts/check_format_version.py'
    }
}

$extensions = @('*.cpp', '*.hpp', '*.h', '*.cc', '*.cxx')
$files = Get-ChildItem -Path $root -Recurse -File -Include $extensions |
    Where-Object {
        $_.FullName -notmatch '\\build(\\|$)' -and
        # Golden files are generated text and must stay byte-identical to the generator output.
        $_.FullName -notmatch '\\tests\\golden\\' -and
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

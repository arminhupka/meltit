# Buduje MELTIT (Standalone + VST3) na Windows i kopiuje gotowe pliki do dist/windows.
# Uzycie:  powershell -ExecutionPolicy Bypass -File .\build-windows.ps1 [-SkipTests] [-Clean]
param(
    [switch]$SkipTests,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Set-Location $root

function Invoke-Step([string]$what, [scriptblock]$cmd) {
    Write-Host "==> $what" -ForegroundColor Cyan
    & $cmd
    if ($LASTEXITCODE -ne 0) { throw "Blad: $what (kod $LASTEXITCODE)" }
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'Nie znaleziono cmake w PATH (wymagany 3.22+).'
}

$buildDir = Join-Path $root 'build/windows-x64'
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host '==> Czyszcze katalog build' -ForegroundColor Cyan
    Remove-Item $buildDir -Recurse -Force
}

Invoke-Step 'Konfiguracja (preset windows-x64)' { cmake --preset windows-x64 }
Invoke-Step 'Budowanie (Release)' { cmake --build --preset windows-release }
if (-not $SkipTests) {
    Invoke-Step 'Testy' { ctest --preset windows-release }
}

$artefacts = Join-Path $buildDir 'TapeMultiFx_artefacts/Release'
$exe  = Join-Path $artefacts 'Standalone/MELTIT.exe'
$vst3 = Join-Path $artefacts 'VST3/MELTIT.vst3'
foreach ($p in @($exe, $vst3)) {
    if (-not (Test-Path $p)) { throw "Brak pliku po buildzie: $p" }
}

$out = Join-Path $root 'dist/windows'
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Path "$out/Standalone", "$out/VST3" -Force | Out-Null

Copy-Item $exe "$out/Standalone/"
Copy-Item $vst3 "$out/VST3/" -Recurse
Copy-Item (Join-Path $root 'Assets/Fonts/*.txt') $out -ErrorAction SilentlyContinue

Write-Host ''
Write-Host "Gotowe: $out" -ForegroundColor Green
Get-ChildItem $out -Recurse -File | ForEach-Object {
    '  ' + $_.FullName.Substring($out.Length + 1)
}

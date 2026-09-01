<#
.SYNOPSIS
    Stage the Forge payload and list which of it is ours.

.DESCRIPTION
    Installs the built tree into stage/, then writes build/payload-ours.txt
    naming only the binaries Locke Werks produced.

    That second file is the point. The signing action takes a folder, not a
    filter, and most of the payload belongs to Qt. Signing the whole staging
    directory would be signing someone else's binaries; signing nothing would
    leave ours unsigned on disk, because Forge extracts payload members verbatim
    and signing the finished installer does nothing for what is inside it.
#>
[CmdletBinding()]
param(
    [string] $BuildDir = 'build/release',
    [string] $Config   = 'Release',
    [string] $StageDir = 'stage'
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

if (Test-Path $StageDir) {
    Remove-Item $StageDir -Recurse -Force
}

cmake --install $BuildDir --config $Config --prefix $StageDir
if ($LASTEXITCODE -ne 0) { throw "cmake --install failed with $LASTEXITCODE" }

# Ours, and only ours. Everything else in stage/ is Qt.
$ours = @('ProjectMan.exe', 'pm.exe')

$found = foreach ($name in $ours) {
    $path = Join-Path $StageDir $name
    if (-not (Test-Path $path)) { throw "staging is missing $name" }
    (Resolve-Path $path).Path
}

New-Item -ItemType Directory -Force -Path 'build' | Out-Null
$found | Set-Content 'build/payload-ours.txt' -Encoding utf8

Write-Host ''
Write-Host 'staged:'
Get-ChildItem $StageDir -Recurse -File |
    Sort-Object -Property Length -Descending |
    Select-Object -First 12 |
    ForEach-Object { '  {0,10:N0}  {1}' -f $_.Length, $_.FullName.Substring($repo.Length + 1) }

$total = (Get-ChildItem $StageDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ''
Write-Host ('  {0} files, {1:N1} MB total' -f
    (Get-ChildItem $StageDir -Recurse -File).Count, ($total / 1MB))
Write-Host ''
Write-Host 'ours (to be signed):'
$found | ForEach-Object { "  $_" }

<#
.SYNOPSIS
    Stage both Forge payloads and list which of them is ours.

.DESCRIPTION
    Two products ship from this repository, so there are two payloads.

    stage/     ProjectMan: both front ends plus the Qt runtime.
    stage-mcp/ ProjectMan MCP: one binary, and deliberately nothing else.

    The MCP server is its own install component, so the default install pass
    does not place it in stage/. That separation is the point: ProjectMan is
    documented as not shipping the MCP integration, and a payload that quietly
    carried it would make that documentation false.

    build/payload-ours.txt names every binary Locke Werks produced, across both
    payloads. The signing action takes a folder rather than a filter and most of
    stage/ belongs to Qt, so ours are collected into one directory to be signed
    and copied back afterwards by the paths in that file.
#>
[CmdletBinding()]
param(
    [string] $BuildDir    = 'build/release',
    [string] $Config      = 'Release',
    [string] $StageDir    = 'stage',
    [string] $McpStageDir = 'stage-mcp'
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

foreach ($dir in @($StageDir, $McpStageDir)) {
    if (Test-Path $dir) {
        Remove-Item $dir -Recurse -Force
    }
}

# The default component: everything except the MCP server, which is marked
# EXCLUDE_FROM_ALL in src/mcp/CMakeLists.txt.
cmake --install $BuildDir --config $Config --prefix $StageDir
if ($LASTEXITCODE -ne 0) { throw "cmake --install failed with $LASTEXITCODE" }

cmake --install $BuildDir --config $Config --prefix $McpStageDir --component mcp
if ($LASTEXITCODE -ne 0) { throw "cmake --install --component mcp failed with $LASTEXITCODE" }

$expected = @{
    $StageDir    = @('ProjectMan.exe', 'pm.exe')
    $McpStageDir = @('pm-mcp.exe')
}

$found = foreach ($dir in $expected.Keys) {
    foreach ($name in $expected[$dir]) {
        $path = Join-Path $dir $name
        if (-not (Test-Path $path)) { throw "staging is missing $path" }
        (Resolve-Path $path).Path
    }
}

# A payload that carried the other product's binary would contradict what each
# installer says it contains, so this is checked rather than assumed.
if (Test-Path (Join-Path $StageDir 'pm-mcp.exe')) {
    throw 'pm-mcp.exe is in the ProjectMan payload; it belongs only to the MCP installer'
}

New-Item -ItemType Directory -Force -Path 'build' | Out-Null
$found | Set-Content 'build/payload-ours.txt' -Encoding utf8

foreach ($dir in @($StageDir, $McpStageDir)) {
    $files = Get-ChildItem $dir -Recurse -File
    $total = ($files | Measure-Object -Property Length -Sum).Sum
    Write-Host ''
    Write-Host "${dir}:"
    $files |
        Sort-Object -Property Length -Descending |
        Select-Object -First 8 |
        ForEach-Object { '  {0,10:N0}  {1}' -f $_.Length, $_.FullName.Substring($repo.Length + 1) }
    Write-Host ('  {0} files, {1:N1} MB total' -f $files.Count, ($total / 1MB))
}

Write-Host ''
Write-Host 'ours (to be signed):'
$found | ForEach-Object { "  $_" }

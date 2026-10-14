param(
    [string]$ZigPath = $env:TINY_ZIG,
    [string]$OutputPath = (Join-Path $PSScriptRoot 'dist/Tiny.exe'),
    [switch]$RunTests
)
$ErrorActionPreference = 'Stop'
if (-not $ZigPath) {
    $compiler = Get-Command zig -ErrorAction SilentlyContinue
    if (-not $compiler) { throw 'Provide -ZigPath pointing to zig.exe (tested with Zig 0.17.0).' }
    $ZigPath = $compiler.Source
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputPath) | Out-Null
$resourceOutput = Join-Path (Split-Path -Parent $OutputPath) 'tiny-resource.o'
& $ZigPath rc /:auto-includes none /i $PSScriptRoot /fo $resourceOutput (Join-Path $PSScriptRoot 'tiny.rc')
if ($LASTEXITCODE -ne 0) { throw 'Resource build failed.' }
$libraries = @('-luser32','-lshell32','-lgdi32','-ladvapi32')
if ($RunTests) {
    $testOutput = Join-Path (Split-Path -Parent $OutputPath) 'quota-tests.exe'
    & $ZigPath cc (Join-Path $PSScriptRoot 'tests/test.c') $resourceOutput -target x86_64-windows-gnu -std=c11 -O2 -UNDEBUG -Wall -Wextra -Werror -Wno-unused-function @libraries -o $testOutput
    if ($LASTEXITCODE -ne 0) { throw 'Test build failed.' }
    & $testOutput
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
& $ZigPath cc (Join-Path $PSScriptRoot 'tiny.c') $resourceOutput -target x86_64-windows-gnu -std=c11 -Os -s -Wall -Wextra -Werror -municode '-Wl,--subsystem,windows' @libraries -o $OutputPath
if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }
Remove-Item -LiteralPath $resourceOutput
Get-Item -LiteralPath $OutputPath | Select-Object FullName,Length

[CmdletBinding()]
param(
    [ValidateSet('debug', 'releasedbg')]
    [string] $Mode = 'releasedbg'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'prepare_commonlib.ps1')
Push-Location $projectRoot
try {
    & xmake f -m $Mode -y
    if ($LASTEXITCODE -ne 0) {
        throw "xmake configuration failed with exit code $LASTEXITCODE."
    }

    & xmake build ClaimablePOIsCoreTests
    if ($LASTEXITCODE -ne 0) {
        throw "Core test build failed with exit code $LASTEXITCODE."
    }

    & xmake run ClaimablePOIsCoreTests
    if ($LASTEXITCODE -ne 0) {
        throw "Core tests failed with exit code $LASTEXITCODE."
    }

    & xmake build ClaimablePOIsRefCountTests
    if ($LASTEXITCODE -ne 0) { throw 'Form refcount test build failed.' }
    & xmake run ClaimablePOIsRefCountTests
    if ($LASTEXITCODE -ne 0) { throw 'Form refcount regression tests failed.' }
}
finally {
    Pop-Location
    & (Join-Path $PSScriptRoot 'prepare_commonlib.ps1') -Restore
}

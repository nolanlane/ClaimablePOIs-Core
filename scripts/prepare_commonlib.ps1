[CmdletBinding()]
param([switch]$Restore)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$dependency = Join-Path $projectRoot 'external\CommonLibSF'
$patch = Join-Path $projectRoot 'patches\commonlibsf-1.16.244-refcounts.patch'
$expectedRevision = '6eeb9e60590db21b6f983d487b6b2d51a0fb6726'
$revision = (& git -C $dependency rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $revision -ne $expectedRevision) {
    throw 'Refcount patch requires the pinned CommonLibSF revision.'
}

# Normalize only line endings; Git's Windows checkout policy may use CRLF.
function Get-TextHash([string] $Path) {
    $bytes = [Text.Encoding]::UTF8.GetBytes([IO.File]::ReadAllText($Path).Replace("`r`n", "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '') }
    finally { $sha.Dispose() }
}

$expected = @{
    'include/RE/T/TESFormRefCount.h' = 'FB69A1181AA875FC9E14F8C79499845634306B010DBEBE02AD25B4FF9E330A32'
    'include/RE/IDs.h' = '4D0ABCE600C6488C38114257104D1D70DDA56FF381326543A35C0F38512A927F'
}
if ((Get-TextHash $patch) -ne '338B1FA282E47F7368ED321279C57D28DCCD76A3599558D7BDA71D220490FFE5') {
    throw 'CommonLibSF refcount patch hash mismatch.'
}
$alreadyPatched = $true
foreach ($relative in $expected.Keys) {
    if ((Get-TextHash (Join-Path $dependency $relative)) -ne $expected[$relative]) { $alreadyPatched = $false }
}

if ($Restore) {
    if (-not $alreadyPatched) {
        throw 'Refusing to restore CommonLibSF because the expected generated patch is not present.'
    }
    & git -C $dependency apply --reverse --check $patch
    if ($LASTEXITCODE -ne 0) { throw 'CommonLibSF patch restore preflight failed.' }
    & git -C $dependency apply --reverse $patch
    if ($LASTEXITCODE -ne 0) { throw 'CommonLibSF patch restore failed.' }
    & git -C $dependency diff --quiet HEAD -- @($expected.Keys)
    if ($LASTEXITCODE -ne 0) { throw 'CommonLibSF did not return to the pinned clean state.' }
    Write-Output 'FormRefCountABI-v1: generated CommonLibSF patch restored to the clean pinned checkout.'
    return
}

if (-not $alreadyPatched) {
    # Never overwrite user edits or apply onto a partial/different patch.
    & git -C $dependency diff --quiet HEAD -- @($expected.Keys)
    if ($LASTEXITCODE -ne 0) { throw 'CommonLibSF refcount inputs contain unexpected edits; preserved without modification.' }
    & git -C $dependency apply --check $patch
    if ($LASTEXITCODE -ne 0) { throw 'CommonLibSF refcount patch preflight failed.' }
    & git -C $dependency apply $patch
    if ($LASTEXITCODE -ne 0) { throw 'CommonLibSF refcount patch application failed.' }
}
foreach ($relative in $expected.Keys) {
    if ((Get-TextHash (Join-Path $dependency $relative)) -ne $expected[$relative]) {
        throw "Patched CommonLibSF input hash mismatch: $relative"
    }
}
Write-Output 'FormRefCountABI-v1: pinned CommonLibSF compatibility patch verified.'

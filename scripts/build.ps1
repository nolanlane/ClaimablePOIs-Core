[CmdletBinding()]
param(
    [ValidateSet('debug', 'releasedbg')]
    [string] $Mode = 'releasedbg',
    [string] $StagingRoot = (Join-Path $PSScriptRoot '..\staging')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'common.ps1')
$staging = Get-CanonicalPath $StagingRoot
Assert-WorkspaceChild -Path $staging -Workspace $projectRoot
Assert-NoExistingReparsePoint -Path $staging
Assert-NotInsideStarfieldInstall -Path $staging
Assert-NoExistingReparsePoint -Path (Join-Path $staging 'Data\SFSE\Plugins\ClaimablePOIs.dll')
$expectedCommonLib = '6eeb9e60590db21b6f983d487b6b2d51a0fb6726'

function Assert-GitRevision {
    param(
        [Parameter(Mandatory)] [string] $RelativePath,
        [Parameter(Mandatory)] [string] $Expected
    )

    $actual = (& git -C (Join-Path $projectRoot $RelativePath) rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $actual -ne $Expected) {
        throw "Dependency revision mismatch at $RelativePath. Expected $Expected; found $actual."
    }
}

Assert-GitRevision -RelativePath 'external/CommonLibSF' -Expected $expectedCommonLib
& (Join-Path $PSScriptRoot 'prepare_commonlib.ps1')

# CommonLib's plugin rule auto-deploys when either variable is present. Builds
# for this repository must remain isolated from the live game and mod manager.
Remove-Item Env:XSE_SF_MODS_PATH -ErrorAction SilentlyContinue
Remove-Item Env:XSE_SF_GAME_PATH -ErrorAction SilentlyContinue

Push-Location $projectRoot
try {
    & xmake f -m $Mode -y
    if ($LASTEXITCODE -ne 0) {
        throw "xmake configuration failed with exit code $LASTEXITCODE."
    }

    & xmake -r
    if ($LASTEXITCODE -ne 0) {
        throw "xmake build failed with exit code $LASTEXITCODE."
    }

    $sourceDll = Join-Path $projectRoot "build\windows\x64\$Mode\ClaimablePOIs.dll"
    if (-not (Test-Path -LiteralPath $sourceDll -PathType Leaf)) {
        throw "Expected build output is missing: $sourceDll"
    }

    $stagingPlugins = Join-Path $staging 'Data\SFSE\Plugins'
    [System.IO.Directory]::CreateDirectory($stagingPlugins) | Out-Null
    $stagedDll = Join-Path $stagingPlugins 'ClaimablePOIs.dll'
    Copy-Item -LiteralPath $sourceDll -Destination $stagedDll -Force

    $sourceDllHash = (Get-FileHash -LiteralPath $sourceDll -Algorithm SHA256).Hash
    $stagedDllHash = (Get-FileHash -LiteralPath $stagedDll -Algorithm SHA256).Hash
    if ($sourceDllHash -ne $stagedDllHash) {
        throw 'Staged DLL failed SHA-256 verification.'
    }

    [pscustomobject]@{
        Dll = $stagedDll
        DllSha256 = $stagedDllHash
    } | Format-List
}
finally {
    Pop-Location
    & (Join-Path $PSScriptRoot 'prepare_commonlib.ps1') -Restore
}

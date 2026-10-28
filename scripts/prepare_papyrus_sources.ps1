[CmdletBinding()]
param(
    [string] $ContentResourcesPath = 'S:\SteamLibrary\steamapps\common\Starfield\Tools\ContentResources.zip',
    [string] $OutputRoot,
    [switch] $Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$projectRoot = Get-CanonicalPath (Join-Path $PSScriptRoot '..')
if (-not $OutputRoot) {
    $OutputRoot = Join-Path $projectRoot 'tools\PapyrusSource'
}
$outputPath = Get-CanonicalPath $OutputRoot
Assert-WorkspaceChild -Path $outputPath -Workspace $projectRoot
Assert-NoExistingReparsePoint -Path $outputPath
Assert-NotInsideStarfieldInstall -Path $outputPath

if (-not (Test-Path -LiteralPath $ContentResourcesPath -PathType Leaf)) {
    throw "Installed Starfield ContentResources.zip not found: $ContentResourcesPath"
}
$basePath = Join-Path $outputPath 'Base'
$flagsPath = Join-Path $outputPath 'Starfield_Papyrus_Flags.flg'
if ((Test-Path -LiteralPath $basePath -PathType Container) -and
    (Test-Path -LiteralPath $flagsPath -PathType Leaf) -and -not $Force) {
    Write-Output "Papyrus sources already prepared: $outputPath"
    return
}
if ((Test-Path -LiteralPath $outputPath) -and -not $Force) {
    throw "Output exists but is incomplete; inspect it or rerun with -Force: $outputPath"
}

$temporary = Join-Path (Split-Path -Parent $outputPath) ('.PapyrusSource-' + [guid]::NewGuid().ToString('N'))
$prepared = Join-Path $temporary 'prepared'
try {
    [System.IO.Directory]::CreateDirectory($temporary) | Out-Null
    & tar -xf $ContentResourcesPath -C $temporary 'Scripts/Source'
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to extract Scripts/Source from ContentResources.zip: $LASTEXITCODE"
    }
    $source = Join-Path $temporary 'Scripts\Source'
    $sourceFlags = Join-Path $source 'Starfield_Papyrus_Flags.flg'
    if (-not (Test-Path -LiteralPath $source -PathType Container) -or
        -not (Test-Path -LiteralPath $sourceFlags -PathType Leaf)) {
        throw 'ContentResources.zip did not contain the expected Scripts/Source tree and flags file.'
    }
    [System.IO.Directory]::CreateDirectory($prepared) | Out-Null
    [System.IO.Directory]::Move($source, (Join-Path $prepared 'Base'))
    Copy-Item -LiteralPath (Join-Path $prepared 'Base\Starfield_Papyrus_Flags.flg') -Destination (Join-Path $prepared 'Starfield_Papyrus_Flags.flg')
    $metadata = [ordered]@{
        source = (Get-CanonicalPath $ContentResourcesPath)
        source_sha256 = (Get-FileHash -LiteralPath $ContentResourcesPath -Algorithm SHA256).Hash
        prepared_utc = [DateTime]::UtcNow.ToString('o')
    }
    Write-AtomicJson -Path (Join-Path $prepared 'SOURCE.json') -Value $metadata
    if (Test-Path -LiteralPath $outputPath) {
        [System.IO.Directory]::Delete($outputPath, $true)
    }
    [System.IO.Directory]::Move($prepared, $outputPath)
    $pscCount = @(Get-ChildItem -LiteralPath (Join-Path $outputPath 'Base') -Recurse -File -Filter *.psc).Count
    Write-Output "Prepared $pscCount official Papyrus sources at $outputPath"
}
finally {
    if (Test-Path -LiteralPath $temporary) {
        [System.IO.Directory]::Delete($temporary, $true)
    }
}

[CmdletBinding()]
param(
    [string]$OutputDir = (Join-Path $PSScriptRoot '..\staging\Data'),
    [ValidateRange(30, 600)][int]$TimeoutSeconds = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common.ps1')

$projectRoot = Get-CanonicalPath (Join-Path $PSScriptRoot '..')
. (Join-Path $PSScriptRoot 'dotnet.ps1')
$null = Initialize-PinnedDotNet -Workspace $projectRoot
$toolProject = Join-Path $projectRoot 'tools\ClaimablePOIsGen\ClaimablePOIsGen.csproj'
$outputDirPath = Get-CanonicalPath $OutputDir

Assert-WorkspaceChild -Path $outputDirPath -Workspace $projectRoot
Assert-NoExistingReparsePoint -Path $outputDirPath
Assert-NotInsideStarfieldInstall -Path $outputDirPath

$esmPath = Join-Path $outputDirPath 'ClaimablePOIs.esm'
if (Test-Path -LiteralPath $esmPath -PathType Leaf) {
    throw "Refusing to overwrite an existing ESM: $esmPath"
}

[System.IO.Directory]::CreateDirectory($outputDirPath) | Out-Null

Write-Host "Building ClaimablePOIsGen tool..."
& dotnet build $toolProject --configuration Release --nologo 2>&1 | Write-Host
if ($LASTEXITCODE -ne 0) {
    throw "ClaimablePOIsGen build failed with exit code $LASTEXITCODE."
}

$toolDll = Join-Path $projectRoot 'tools\ClaimablePOIsGen\bin\Release\net10.0\ClaimablePOIsGen.dll'
if (-not (Test-Path -LiteralPath $toolDll -PathType Leaf)) {
    throw "Generator DLL not found after build: $toolDll"
}

Write-Host "Generating ClaimablePOIs.esm into $outputDirPath..."
$stdout = & dotnet $toolDll generate --output $outputDirPath 2>&1
$exitCode = $LASTEXITCODE

$stdout | Write-Host

if ($exitCode -ne 0) {
    throw "ESM generation failed with exit code $exitCode."
}

if (-not (Test-Path -LiteralPath $esmPath -PathType Leaf)) {
    throw "ESM was not produced at the expected path: $esmPath"
}

$esmHash = (Get-FileHash -LiteralPath $esmPath -Algorithm SHA256).Hash
$esmSize = (Get-Item -LiteralPath $esmPath).Length

[pscustomobject]@{
    Path = $esmPath
    Size = $esmSize
    SHA256 = $esmHash
} | Format-List

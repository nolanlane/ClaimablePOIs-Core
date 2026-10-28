[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InputPlugin,
    [string]$ArtifactRoot = (Join-Path $PSScriptRoot '..\staging\validation'),
    [ValidateRange(30, 600)][int]$TimeoutSeconds = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'common.ps1')

$projectRoot = Get-CanonicalPath (Join-Path $PSScriptRoot '..')
. (Join-Path $PSScriptRoot 'dotnet.ps1')
$null = Initialize-PinnedDotNet -Workspace $projectRoot
$toolProject = Join-Path $projectRoot 'tools\ClaimablePOIsGen\ClaimablePOIsGen.csproj'
$artifactRootPath = Get-CanonicalPath $ArtifactRoot
$inputPluginPath = Get-CanonicalPath $InputPlugin

Assert-WorkspaceChild -Path $artifactRootPath -Workspace $projectRoot
Assert-NoExistingReparsePoint -Path $artifactRootPath
Assert-NotInsideStarfieldInstall -Path $artifactRootPath

if (-not (Test-Path -LiteralPath $inputPluginPath -PathType Leaf)) {
    throw "Input plugin not found: $inputPluginPath"
}
if ((Split-Path -Leaf $inputPluginPath) -cne 'ClaimablePOIs.esm') {
    throw 'Input plugin must be named exactly ClaimablePOIs.esm.'
}

Assert-NotInsideStarfieldInstall -Path $inputPluginPath

[System.IO.Directory]::CreateDirectory($artifactRootPath) | Out-Null

Write-Host "Building ClaimablePOIsGen tool..."
& dotnet build $toolProject --configuration Release --nologo 2>&1 | Write-Host
if ($LASTEXITCODE -ne 0) {
    throw "ClaimablePOIsGen build failed with exit code $LASTEXITCODE."
}

$toolDll = Join-Path $projectRoot 'tools\ClaimablePOIsGen\bin\Release\net10.0\ClaimablePOIsGen.dll'
if (-not (Test-Path -LiteralPath $toolDll -PathType Leaf)) {
    throw "Validator DLL not found after build: $toolDll"
}

Write-Host "Validating $inputPluginPath..."
$resultFile = Join-Path $artifactRootPath 'tool-result.json'
& dotnet $toolDll validate --input $inputPluginPath --result-file $resultFile 2>&1 | Write-Host
$exitCode = $LASTEXITCODE

if ($exitCode -ne 0) {
    throw "ESM validation failed with exit code $exitCode."
}

if (-not (Test-Path -LiteralPath $resultFile -PathType Leaf)) {
    throw "Validator did not produce a result file: $resultFile"
}

$validation = Get-Content -LiteralPath $resultFile -Raw | ConvertFrom-Json

$reportPath = Join-Path $artifactRootPath 'esm-validation.json'
$report = [ordered]@{
    schema = 'claimable-pois.esm-validation.v1'
    status = 'passed'
    filename = 'ClaimablePOIs.esm'
    validation_passed = $true
    esm_sha256 = $validation.data.esm_sha256
    validated_at = [DateTime]::UtcNow.ToString('o')
    input_path = $inputPluginPath
}
Write-AtomicJson -Path $reportPath -Value $report

Write-Host "Validation report written to: $reportPath"
$report | ConvertTo-Json -Depth 8

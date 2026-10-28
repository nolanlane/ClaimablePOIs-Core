[CmdletBinding()]
param(
    [string]$ClaimablePath = (Join-Path $PSScriptRoot '..\staging\Data\ClaimablePOIs.esm')
)
. (Join-Path $PSScriptRoot 'common.ps1')
$workspace = Get-CanonicalPath (Join-Path $PSScriptRoot '..')
. (Join-Path $PSScriptRoot 'dotnet.ps1')
$null = Initialize-PinnedDotNet -Workspace $workspace
$output = Join-Path $workspace ('scratch\generator-tests-' + [guid]::NewGuid().ToString('N'))
Push-Location $workspace
try {
    & dotnet run --project 'tests\GeneratorRegressionTests\GeneratorRegressionTests.csproj' --configuration Release -- --core $ClaimablePath $output
    if ($LASTEXITCODE -ne 0) { throw "Generator regressions failed. Evidence: $output" }
} finally { Pop-Location }

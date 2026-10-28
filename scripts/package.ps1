[CmdletBinding()]
param(
    [string]$StagingRoot = (Join-Path $PSScriptRoot '..\staging'),
    [string]$OutputRoot = (Join-Path $PSScriptRoot '..\package'),
    [switch]$CoreOnly
)
. (Join-Path $PSScriptRoot 'artifacts.ps1')
$workspace = Get-CanonicalPath (Join-Path $PSScriptRoot '..')
$staging = Get-CanonicalPath $StagingRoot
$output = Get-CanonicalPath $OutputRoot
foreach ($path in @($staging, $output)) {
    Assert-WorkspaceChild -Path $path -Workspace $workspace
    Assert-NoExistingReparsePoint -Path $path
    Assert-NotInsideStarfieldInstall -Path $path
}
if ((Split-Path -Leaf $output) -cne 'package') { throw 'OutputRoot must be a workspace child named package.' }
if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite existing package: $output" }
$manifest = Get-Content -LiteralPath (Join-Path $staging 'validation\candidate.json') -Raw | ConvertFrom-Json
if ($manifest.source_identity -cne (Get-SourceIdentity -Workspace $workspace)) { throw 'Candidate build inputs differ from current source. Rebuild the candidate.' }
$requiredGates = @(Get-RequiredCandidateGates -Manifest $manifest)
foreach ($gate in ($requiredGates | Select-Object -Unique)) {
    if (@($manifest.gates | Where-Object { $_.name -ceq $gate -and $_.status -ceq 'passed' }).Count -ne 1) { throw "Missing successful gate: $gate" }
}
$data = Join-Path $staging 'Data'
Assert-ArtifactManifest -DataRoot $data -Manifest $manifest
$validation = Get-Content -LiteralPath (Join-Path $staging 'validation\esm-validation.json') -Raw | ConvertFrom-Json
if ($validation.schema -cne 'claimable-pois.esm-validation.v1' -or $validation.status -cne 'passed' -or
    $validation.validation_passed -ne $true -or $validation.esm_sha256 -ine (Get-FileHash -LiteralPath (Join-Path $data 'ClaimablePOIs.esm')).Hash) { throw 'Core ESM validation does not match candidate.' }
if ($CoreOnly) {
    if ($manifest.schema -cne 'claimable-pois.candidate.v2') { throw 'CoreOnly requires an explicit-module v2 candidate.' }
    $corePaths = @(Get-ModuleArtifactPaths -Modules @('core'))
    $manifest.modules = @('core')
    $manifest.artifacts = @($manifest.artifacts | Where-Object { $_.path -cin $corePaths })
    $manifest.inputs = @($manifest.inputs | Where-Object { $_.name -ceq 'Starfield.esm' })
    $manifest | Add-Member -NotePropertyName parent_receipt_sha256 -NotePropertyValue (Get-FileHash -LiteralPath (Join-Path $staging 'validation\candidate.json')).Hash -Force
    Assert-ArtifactManifest -DataRoot $data -Manifest $manifest
}
foreach ($artifact in $manifest.artifacts) {
    $destination = Join-Path $output "Data\$($artifact.path)"
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
    Copy-Item -LiteralPath (Join-Path $data $artifact.path) -Destination $destination
}
Assert-ArtifactManifest -DataRoot (Join-Path $output 'Data') -Manifest $manifest
Write-AtomicJson -Path (Join-Path $output 'candidate.json') -Value $manifest
Write-Host "Packaged and verified $($manifest.artifacts.Count) artifacts from $($manifest.source_commit)"

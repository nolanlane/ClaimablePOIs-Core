[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Version,

    [string]$PackageRoot = (Join-Path $PSScriptRoot '..\package'),
    [string]$StagingRoot = (Join-Path $PSScriptRoot '..\staging'),
    [string]$DistRoot = (Join-Path $PSScriptRoot '..\dist'),
    [string]$EsmReport = (Join-Path $StagingRoot 'validation\esm-validation.json')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-CanonicalPath {
    param([Parameter(Mandatory)][string]$Path)

    $providerPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    return [System.IO.Path]::GetFullPath($providerPath).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    )
}

function Assert-WorkspaceChild {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Workspace
    )

    $prefix = $Workspace + [System.IO.Path]::DirectorySeparatorChar
    if (-not $Path.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path must remain inside the workspace: $Path"
    }
}

$cleanVersion = $Version.Trim()
if ($cleanVersion.StartsWith('v', [System.StringComparison]::OrdinalIgnoreCase)) {
    $cleanVersion = $cleanVersion.Substring(1)
}
if ($cleanVersion -notmatch '^(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$') {
    throw "Invalid semantic version format: '$Version'. Expected semver (e.g., '0.1.0-alpha.1' or 'v0.1.0-alpha.1')."
}

$projectRoot = Get-CanonicalPath (Join-Path $PSScriptRoot '..')
$packageRootPath = Get-CanonicalPath $PackageRoot
$stagingRootPath = Get-CanonicalPath $StagingRoot
$distRootPath = Get-CanonicalPath $DistRoot
$esmReportPath = Get-CanonicalPath $EsmReport

Assert-WorkspaceChild -Path $packageRootPath -Workspace $projectRoot
Assert-WorkspaceChild -Path $stagingRootPath -Workspace $projectRoot
Assert-WorkspaceChild -Path $distRootPath -Workspace $projectRoot
Assert-WorkspaceChild -Path $esmReportPath -Workspace $projectRoot

$packageData = Join-Path $packageRootPath 'Data'
if (-not (Test-Path -LiteralPath $packageData -PathType Container)) {
    throw "Package Data directory not found: $packageData"
}

. (Join-Path $PSScriptRoot 'artifacts.ps1')
Assert-NoExistingReparsePoint -Path $packageRootPath
Assert-NoExistingReparsePoint -Path $distRootPath
Assert-NotInsideStarfieldInstall -Path $distRootPath
$manifest = Get-Content -LiteralPath (Join-Path $packageRootPath 'candidate.json') -Raw | ConvertFrom-Json
Assert-ArtifactManifest -DataRoot $packageData -Manifest $manifest
if ($manifest.source_identity -cne (Get-SourceIdentity -Workspace $projectRoot)) { throw 'Release source differs from candidate; rebuild before packaging.' }
foreach ($gate in (Get-RequiredCandidateGates -Manifest $manifest)) {
    if (@($manifest.gates | Where-Object { $_.name -ceq $gate -and $_.status -ceq 'passed' }).Count -ne 1) { throw "Missing successful release gate: $gate" }
}
$expectedManifest = @($manifest.artifacts.path)
$installerSources = [ordered]@{
    'README.md' = Join-Path $projectRoot 'README.md'
    'LICENSE' = Join-Path $projectRoot 'LICENSE'
    'EXCEPTIONS' = Join-Path $projectRoot 'EXCEPTIONS'
    'THIRD-PARTY-NOTICES.md' = Join-Path $projectRoot 'THIRD-PARTY-NOTICES.md'
}

$actualFiles = @(Get-ChildItem -LiteralPath $packageRootPath -File -Recurse)
if ($actualFiles.Count -ne $expectedManifest.Count + 1 -or
    @(Get-ChildItem -LiteralPath $packageData -File -Recurse).Count -ne $expectedManifest.Count) {
    throw 'Package contains files outside the verified manifest and candidate.json.'
}
if (-not (Test-Path -LiteralPath $esmReportPath -PathType Leaf)) {
    throw "ESM validation report not found: $esmReportPath"
}

$validation = Get-Content -LiteralPath $esmReportPath -Raw | ConvertFrom-Json
if (($validation.schema -cne 'claimable-pois.esm-validation.v1') -or
    ($validation.status -cne 'passed') -or
    ($validation.filename -cne 'ClaimablePOIs.esm') -or
    ($validation.validation_passed -ne $true)) {
    throw "ESM validation report does not authorize release packaging: $esmReportPath"
}

$declaredEsmHash = [string]$validation.esm_sha256
if ($declaredEsmHash -notmatch '^[0-9A-Fa-f]{64}$') {
    throw 'ESM validation report is missing a valid SHA-256.'
}

$packageEsmPath = Join-Path $packageData 'ClaimablePOIs.esm'
$actualPackageEsmHash = (Get-FileHash -LiteralPath $packageEsmPath -Algorithm SHA256).Hash
if ($actualPackageEsmHash -ine $declaredEsmHash) {
    throw "Packaged ESM hash mismatch. Expected $declaredEsmHash (from validation report); found $actualPackageEsmHash."
}

$archiveName = "ClaimablePOIs-v$cleanVersion.zip"
$archivePath = Join-Path $distRootPath $archiveName
$sidecarName = "$archiveName.sha256"
$sidecarPath = Join-Path $distRootPath $sidecarName

if (Test-Path -LiteralPath $archivePath) {
    throw "Refusing to overwrite existing release archive: $archivePath"
}
if (Test-Path -LiteralPath $sidecarPath) {
    throw "Refusing to overwrite existing SHA-256 sidecar: $sidecarPath"
}

[System.IO.Directory]::CreateDirectory($distRootPath) | Out-Null

Add-Type -AssemblyName System.IO.Compression.FileSystem

$tempZipPath = $archivePath + ".tmp.$([guid]::NewGuid().ToString('N'))"
try {
    $zipArchive = [System.IO.Compression.ZipFile]::Open($tempZipPath, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($rel in ($expectedManifest | Sort-Object)) {
            $sourceFile = Join-Path $packageData $rel
            $prefix = 'Data/'
            $entryPath = $prefix + ($rel -replace '\\', '/')
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zipArchive,
                $sourceFile,
                $entryPath,
                [System.IO.Compression.CompressionLevel]::Optimal
            ) | Out-Null
        }
        foreach ($entryName in $installerSources.Keys) {
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zipArchive, $installerSources[$entryName], $entryName, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }

    } finally {
        $zipArchive.Dispose()
    }

    [System.IO.File]::Move($tempZipPath, $archivePath)
} finally {
    if (Test-Path -LiteralPath $tempZipPath) {
        Remove-Item -LiteralPath $tempZipPath -Force -ErrorAction SilentlyContinue
    }
}

$archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
$sidecarContent = "$archiveHash  $archiveName`r`n"
[System.IO.File]::WriteAllText($sidecarPath, $sidecarContent, [System.Text.UTF8Encoding]::new($false))

[pscustomobject]@{
    Version = $cleanVersion
    Archive = $archivePath
    ArchiveSHA256 = $archiveHash
    Sidecar = $sidecarPath
    FileCount = $expectedManifest.Count + $installerSources.Count
    ESMSHA256 = $actualPackageEsmHash
} | Format-List

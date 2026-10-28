[CmdletBinding()]
param(
    [string]$GamePath = 'S:\SteamLibrary\steamapps\common\Starfield',
    [string]$PackageRoot = (Join-Path $PSScriptRoot '..\package')
)
. (Join-Path $PSScriptRoot 'artifacts.ps1')
$workspace = Get-CanonicalPath (Join-Path $PSScriptRoot '..')
$package = Get-CanonicalPath $PackageRoot
Assert-WorkspaceChild -Path $package -Workspace $workspace
Assert-NoExistingReparsePoint -Path $package
Assert-NotInsideStarfieldInstall -Path $package
$game = Get-CanonicalPath $GamePath
Assert-NoExistingReparsePoint -Path $game
$exe = Join-Path $game 'Starfield.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Starfield executable not found: $exe" }
if ((Get-Item -LiteralPath $exe).VersionInfo.FileVersion -notlike '1.16.244.0*') { throw 'Deployment requires Starfield 1.16.244.0.' }
function Assert-GameClosed {
    if (@(Get-Process -Name Starfield,sfse_loader -ErrorAction Ignore).Count -gt 0) { throw 'Refusing deployment while Starfield/SFSE is running.' }
}
Assert-GameClosed
$data = Join-Path $package 'Data'
$manifest = Get-Content -LiteralPath (Join-Path $package 'candidate.json') -Raw | ConvertFrom-Json
Assert-ArtifactManifest -DataRoot $data -Manifest $manifest
if (@(Get-ChildItem -LiteralPath $data -File -Recurse).Count -ne $manifest.artifacts.Count) { throw 'Unexpected files in package Data.' }
if ($manifest.source_identity -cne (Get-SourceIdentity -Workspace $workspace)) { throw 'Package source identity differs from the current candidate.' }
$live = Join-Path $game 'Data'
if (-not (Test-Path -LiteralPath $live -PathType Container)) { throw 'Starfield Data directory is missing.' }
$knownPaths = @(Get-ModuleArtifactPaths -Modules @('core')) + @(Get-ExcludedArtifactPaths)
if (@($knownPaths |
    Where-Object { (Test-Path -LiteralPath (Join-Path $live $_) -PathType Leaf) -and $_ -notin $manifest.artifacts.path }).Count -gt 0) {
    throw 'Live optional groups would remain outside this package; resolve the installation plan before deployment.'
}
foreach ($input in $manifest.inputs) {
    if ($input.name -cne 'Starfield.esm' -or
        (Get-FileHash -LiteralPath (Join-Path $live $input.name)).Hash -cne $input.sha256) {
        throw "Installed master differs from candidate input: $($input.name)"
    }
}
$rollback = Join-Path $workspace ('scratch\deploy-rollback-' + [guid]::NewGuid().ToString('N'))
Assert-NoExistingReparsePoint -Path $rollback
[System.IO.Directory]::CreateDirectory($rollback) | Out-Null
$previous = foreach ($artifact in $manifest.artifacts) {
    $destination = Get-CanonicalPath (Join-Path $live $artifact.path)
    Assert-WorkspaceChild -Path $destination -Workspace $live
    Assert-NoExistingReparsePoint -Path $destination
    $exists = Test-Path -LiteralPath $destination -PathType Leaf
    $hash = $null
    if ($exists) {
        $backup = Join-Path $rollback "Data\$($artifact.path)"
        [System.IO.Directory]::CreateDirectory((Split-Path -Parent $backup)) | Out-Null
        Copy-Item -LiteralPath $destination -Destination $backup
        $hash = (Get-FileHash -LiteralPath $destination).Hash
        if ((Get-FileHash -LiteralPath $backup).Hash -cne $hash) { throw 'Rollback preflight hash mismatch.' }
    }
    [ordered]@{ path = $artifact.path; existed = $exists; sha256 = $hash }
}
Write-AtomicJson -Path (Join-Path $rollback 'previous.json') -Value @($previous)
# No live mutation occurs until every source and rollback file has been verified.
Assert-GameClosed
Assert-ArtifactManifest -DataRoot $data -Manifest $manifest
$changed = [System.Collections.Generic.List[string]]::new()
try {
    foreach ($artifact in $manifest.artifacts) {
        $destination = Join-Path $live $artifact.path
        [System.IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
        $changed.Add($artifact.path)
        Copy-Item -LiteralPath (Join-Path $data $artifact.path) -Destination $destination -Force
        if ((Get-FileHash -LiteralPath $destination).Hash -cne $artifact.sha256) { throw "Deployment hash mismatch: $($artifact.path)" }
    }
    Assert-ArtifactManifest -DataRoot $live -Manifest $manifest
} catch {
    $failure = $_
    $restoreFailures = [System.Collections.Generic.List[string]]::new()
    foreach ($path in $changed) {
        try {
            $old = $previous | Where-Object { $_.path -ceq $path }
            $destination = Join-Path $live $path
            if ($old.existed) {
                Copy-Item -LiteralPath (Join-Path $rollback "Data\$path") -Destination $destination -Force
                if ((Get-FileHash -LiteralPath $destination).Hash -cne $old.sha256) { throw 'Restoration hash mismatch.' }
            } elseif (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination }
        } catch { $restoreFailures.Add($path) }
    }
    if ($restoreFailures.Count -gt 0) { throw "Deployment failed; rollback incomplete for $($restoreFailures -join ', '). Preserve $rollback. Original failure: $failure" }
    throw "Deployment failed and previous artifacts were restored. Evidence: $rollback. Original failure: $failure"
}
Write-AtomicJson -Path (Join-Path $rollback 'deployed.json') -Value $manifest
Write-Output "Deployed and verified $($manifest.artifacts.Count) artifacts; gameplay pending. Rollback: $rollback"

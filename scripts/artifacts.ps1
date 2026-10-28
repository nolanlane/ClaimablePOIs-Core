. (Join-Path $PSScriptRoot 'common.ps1')

function Get-ModuleArtifactPaths {
    param([Parameter(Mandatory)][string[]]$Modules)
    if ($Modules.Count -ne 1 -or $Modules[0] -cne 'core') { throw 'Modules must contain only core; known, unique module names are required.' }
    $paths = @(
        'ClaimablePOIs.esm', 'SFSE\Plugins\ClaimablePOIs.dll',
        'Scripts\CPOI_Native.pex', 'Scripts\CPOI_ClaimQuestScript.pex',
        'Scripts\CPOI_ClaimBeaconEffect.pex', 'Seq\ClaimablePOIs.esm.seq'
    )
    return $paths
}

function Get-LegacyDashboardArtifactPaths {
    # Stale-install detection only. No dashboard source or runtime is included.
    # These files are never selectable in a new candidate.
    return @(
        'SFSE\Plugins\OSFUI\settings\cpoi.claimablepois.json',
        'SFSE\Plugins\OSFUI\views\cpoi.claimablepois\dashboard\index.html',
        'SFSE\Plugins\OSFUI\views\cpoi.claimablepois\dashboard\main.js',
        'SFSE\Plugins\OSFUI\views\cpoi.claimablepois\dashboard\manifest.json',
        'SFSE\Plugins\OSFUI\views\cpoi.claimablepois\dashboard\style.css'
    )
}

function Get-ExcludedArtifactPaths {
    # Names only: prevent optional/retired files surviving a core deployment.
    return @(
        'ClaimablePOIs-LivingOutposts.esm', 'ClaimablePOIs-LivingOutpostsFix.esm',
        'Seq\ClaimablePOIs-LivingOutpostsFix.esm.seq', 'Scripts\CPOI_LivingOutpostsFixScript.pex',
        'ClaimablePOIs-TinyReasonsPatch.esm', 'ClaimablePOIs-MiniSavePlacement.esm'
    ) + @(Get-LegacyDashboardArtifactPaths)
}

function Get-ManifestArtifactPaths {
    param([Parameter(Mandatory)][string]$DataRoot, [Parameter(Mandatory)][object]$Manifest)
    if ($Manifest.schema -ceq 'claimable-pois.candidate.v2') {
        return Get-ModuleArtifactPaths -Modules @($Manifest.modules)
    }
    throw 'This core repository accepts explicit core-only v2 receipts.'
}

function Get-RequiredCandidateGates {
    param([Parameter(Mandatory)][object]$Manifest)
    $gates = @('native-tests','native-build','core-generation','core-validation','papyrus-build','artifact-pipeline','generator-regressions')
    return $gates | Select-Object -Unique
}

function Get-SourceIdentity {
    param([Parameter(Mandatory)][string]$Workspace)
    $paths = @(& git -C $Workspace ls-files -- src scripts tools/ClaimablePOIsGen tests patches installer LICENSE EXCEPTIONS xmake.lua global.json DEPENDENCIES.md .github/workflows)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify tracked build inputs.' }
    $lines = foreach ($path in ($paths | Sort-Object)) {
        "$path=$((Get-FileHash -LiteralPath (Join-Path $Workspace $path) -Algorithm SHA256).Hash)"
    }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($lines -join "`n")
    return [Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($bytes))
}

function Assert-CandidateSourceCommitted {
    param([Parameter(Mandatory)][string]$Workspace)
    $tracked = @(& git -C $Workspace status --porcelain --untracked-files=no)
    if ($LASTEXITCODE -ne 0 -or $tracked.Count -ne 0) { throw 'Commit the candidate source before building its provenance.' }
    # Unrelated user documents are not build inputs. Do not require their removal,
    # while still rejecting uncommitted files that globbed build rules could consume.
    $untrackedInputs = @(& git -C $Workspace ls-files --others --exclude-standard -- src scripts tools tests patches installer LICENSE EXCEPTIONS .github xmake.lua global.json DEPENDENCIES.md)
    if ($LASTEXITCODE -ne 0 -or $untrackedInputs.Count -ne 0) { throw 'Untracked build inputs must be committed before building candidate provenance.' }
}

function Assert-ArtifactManifest {
    param([Parameter(Mandatory)][string]$DataRoot, [Parameter(Mandatory)][object]$Manifest)
    if ($Manifest.schema -cne 'claimable-pois.candidate.v2' -or $Manifest.status -cne 'gameplay-pending' -or
        $Manifest.source_commit -notmatch '^[0-9a-f]{40}$') { throw 'Invalid candidate manifest.' }
    $expected = @(Get-ManifestArtifactPaths -DataRoot $DataRoot -Manifest $Manifest | Sort-Object)
    $declared = @($Manifest.artifacts.path | Sort-Object)
    if (@(Compare-Object $expected $declared).Count -ne 0 -or @($declared | Select-Object -Unique).Count -ne $declared.Count) {
        throw 'Candidate manifest does not match the core/optional artifact groups.'
    }
    foreach ($artifact in $Manifest.artifacts) {
        $path = Get-CanonicalPath (Join-Path $DataRoot $artifact.path)
        Assert-WorkspaceChild -Path $path -Workspace (Get-CanonicalPath $DataRoot)
        Assert-NoExistingReparsePoint -Path $path
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-Item -LiteralPath $path).Length -ne $artifact.length -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -cne $artifact.sha256) {
            throw "Candidate artifact is missing or changed: $($artifact.path)"
        }
    }
}


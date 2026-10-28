[CmdletBinding()]
param()
. (Join-Path $PSScriptRoot '..\scripts\artifacts.ps1')
$workspace = Get-CanonicalPath (Join-Path $PSScriptRoot '..')
$root = Join-Path $workspace ('scratch\pipeline-tests-' + [guid]::NewGuid().ToString('N'))
[System.IO.Directory]::CreateDirectory($root) | Out-Null
$passed = 0
function Assert-Test {
    param([bool]$Value, [string]$Name)
    if (-not $Value) { throw "FAIL: $Name" }
    $script:passed++
    Write-Host "PASS: $Name"
}
. (Join-Path $PSScriptRoot '..\scripts\dotnet.ps1')
$sdkFixture = Join-Path $root 'sdk-selection'
foreach ($version in @('10.0.402','10.0.401')) {
    $hostRoot = Join-Path $sdkFixture $version
    [IO.Directory]::CreateDirectory((Join-Path $hostRoot "sdk\$version")) | Out-Null
    [IO.File]::WriteAllText((Join-Path $hostRoot 'dotnet.exe'),'inert host')
    [IO.File]::WriteAllText((Join-Path $hostRoot "sdk\$version\dotnet.dll"),'inert SDK')
}
$wrongSdk = Join-Path $sdkFixture '10.0.402'
$pinnedSdk = Join-Path $sdkFixture '10.0.401'
Assert-Test ((Resolve-PinnedDotNetRoot -Version '10.0.401' -CandidateRoots @($wrongSdk,$pinnedSdk)) -ceq $pinnedSdk) 'SDK selector skips newer wrong version and selects exact pin'
Assert-Test ((Resolve-PinnedDotNetRoot -Version '10.0.401' -CandidateRoots @($pinnedSdk,$wrongSdk)) -ceq $pinnedSdk) 'SDK selector respects explicit candidate priority'
$missingSdkRejected = $false
try { Resolve-PinnedDotNetRoot -Version '10.0.400' -CandidateRoots @($wrongSdk,$pinnedSdk) | Out-Null }
catch { $missingSdkRejected = $_.ToString().Contains('before any build') }
Assert-Test $missingSdkRejected 'Missing pinned SDK rejects before build work'
$incompleteSdk = Join-Path $sdkFixture 'incomplete'
[IO.Directory]::CreateDirectory((Join-Path $incompleteSdk 'sdk\10.0.401')) | Out-Null
[IO.File]::WriteAllText((Join-Path $incompleteSdk 'dotnet.exe'),'inert host only')
$incompleteSdkRejected = $false
try { Resolve-PinnedDotNetRoot -Version '10.0.401' -CandidateRoots @($incompleteSdk) | Out-Null }
catch { $incompleteSdkRejected = $true }
Assert-Test $incompleteSdkRejected 'Empty SDK directory cannot satisfy pinned SDK selection'
$sourceFixture = Join-Path $root 'source-provenance'
[System.IO.Directory]::CreateDirectory((Join-Path $sourceFixture 'src')) | Out-Null
& git -C $sourceFixture init --quiet
if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize source fixture.' }
[System.IO.File]::WriteAllText((Join-Path $sourceFixture 'src\fixture.cpp'), 'committed source')
& git -C $sourceFixture add -- src/fixture.cpp
& git -C $sourceFixture -c user.name=Fixture -c user.email=fixture@example.invalid commit --quiet -m 'Inert fixture'
if ($LASTEXITCODE -ne 0) { throw 'Cannot commit source fixture.' }
[System.IO.File]::WriteAllText((Join-Path $sourceFixture 'user-notes.txt'), 'unrelated user work')
Assert-CandidateSourceCommitted -Workspace $sourceFixture
Assert-Test $true 'Unrelated untracked user files do not block committed candidate sources'
[System.IO.File]::WriteAllText((Join-Path $sourceFixture 'src\untracked.cpp'), 'untracked input')
$rejected = $false
try { Assert-CandidateSourceCommitted -Workspace $sourceFixture } catch { $rejected = $_.ToString().Contains('Untracked build inputs') }
Assert-Test $rejected 'Untracked build input rejects candidate provenance'
& git -C $sourceFixture add -- src/untracked.cpp
$rejected = $false
try { Assert-CandidateSourceCommitted -Workspace $sourceFixture } catch { $rejected = $_.ToString().Contains('Commit the candidate source') }
Assert-Test $rejected 'Staged uncommitted build input rejects candidate provenance'
foreach ($unsafe in @($workspace, (Join-Path $workspace '..\outside-workspace'))) {
    $rejected = $false
    try { Assert-WorkspaceChild -Path (Get-CanonicalPath $unsafe) -Workspace $workspace } catch { $rejected = $true }
    Assert-Test $rejected 'Output cannot be workspace root or escape workspace'
}
$target = Join-Path $root 'junction-target'
[IO.Directory]::CreateDirectory($target) | Out-Null
$junction = Join-Path $root 'junction-output'
New-Item -ItemType Junction -Path $junction -Target $target | Out-Null
$rejected = $false
try { Assert-NoExistingReparsePoint -Path (Join-Path $junction 'nested') } catch { $rejected = $true }
Assert-Test $rejected 'Output below a junction is rejected'
foreach ($fixtureName in @('core-rollback')) {
    $fixture = Join-Path $root $fixtureName
    $stage = Join-Path $fixture 'staging'
    $data = Join-Path $stage 'Data'
    [System.IO.Directory]::CreateDirectory((Join-Path $stage 'validation')) | Out-Null
    [System.IO.Directory]::CreateDirectory($data) | Out-Null
    foreach ($path in (Get-ModuleArtifactPaths -Modules @('core'))) {
        $file = Join-Path $data $path
        [System.IO.Directory]::CreateDirectory((Split-Path -Parent $file)) | Out-Null
        [System.IO.File]::WriteAllText($file, "fixture:$path")
    }
    $artifacts = foreach ($path in (Get-ModuleArtifactPaths -Modules @('core'))) {
        [ordered]@{ path = $path; length = (Get-Item -LiteralPath (Join-Path $data $path)).Length; sha256 = (Get-FileHash -LiteralPath (Join-Path $data $path)).Hash }
    }
    $gates = foreach ($name in @('native-tests','native-build','core-generation','core-validation','papyrus-build','generator-regressions','artifact-pipeline')) {
        [ordered]@{ name = $name; status = 'passed' }
    }
    $manifest = [ordered]@{
        schema = 'claimable-pois.candidate.v2'; status = 'gameplay-pending'; modules = @('core')
        source_commit = (& git -C $workspace rev-parse HEAD).Trim(); source_identity = Get-SourceIdentity -Workspace $workspace
        artifacts = @($artifacts); gates = @($gates)
        inputs = @()
    }
    Write-AtomicJson -Path (Join-Path $stage 'validation\candidate.json') -Value $manifest
    Write-AtomicJson -Path (Join-Path $stage 'validation\esm-validation.json') -Value ([ordered]@{
        schema = 'claimable-pois.esm-validation.v1'; status = 'passed'; filename = 'ClaimablePOIs.esm'
        validation_passed = $true; esm_sha256 = (Get-FileHash -LiteralPath (Join-Path $data 'ClaimablePOIs.esm')).Hash
    })
    $package = Join-Path $fixture 'package'
    & (Join-Path $workspace 'scripts\package.ps1') -StagingRoot $stage -OutputRoot $package
    Assert-Test (@(Get-ChildItem -LiteralPath (Join-Path $package 'Data') -File -Recurse).Count -eq 6) "$(Split-Path -Leaf $fixture) package artifact groups"
    & (Join-Path $workspace 'scripts\release.ps1') -Version '0.0.0-test' -PackageRoot $package -StagingRoot $stage -DistRoot (Join-Path $fixture 'dist') | Out-Null
    Assert-Test (Test-Path -LiteralPath (Join-Path $fixture 'dist\ClaimablePOIs-v0.0.0-test.zip')) 'Release accepts verified manifest including optional groups'
    $rejected = $false
    try { & (Join-Path $workspace 'scripts\release.ps1') -Version '0.0.0-test' -PackageRoot $package -StagingRoot $stage -DistRoot (Join-Path $fixture 'dist') | Out-Null } catch { $rejected = $true }
    Assert-Test $rejected 'Release archive cannot be overwritten'

    $fakeGame = Join-Path $fixture 'game'
    $global:cpoiPipelineMockState = @{ game = $fakeGame; copies = 0 }
    [System.IO.Directory]::CreateDirectory((Join-Path $fakeGame 'Data')) | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $fakeGame 'Starfield.exe'), 'fixture-only; never executable')
    foreach ($artifact in $manifest.artifacts) {
        $destination = Join-Path $fakeGame "Data\$($artifact.path)"
        [System.IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
        [System.IO.File]::WriteAllText($destination, "old:$($artifact.path)")
    }
    $before = @{}
    foreach ($artifact in $manifest.artifacts) { $before[$artifact.path] = (Get-FileHash -LiteralPath (Join-Path $fakeGame "Data\$($artifact.path)")).Hash }
    # Scope-local mocks exercise the actual deployment script with inert files.
    function Get-Item {
        param([string]$LiteralPath)
        if ($LiteralPath -eq (Join-Path $global:cpoiPipelineMockState.game 'Starfield.exe')) { return [pscustomobject]@{ VersionInfo = [pscustomobject]@{ FileVersion = '1.16.244.0' } } }
        Microsoft.PowerShell.Management\Get-Item -LiteralPath $LiteralPath
    }
    function Get-Process { param([string[]]$Name, [string]$ErrorAction) }
    $missing = Join-Path $package 'Data\Scripts\CPOI_ClaimBeaconEffect.pex'
    $bytes = [System.IO.File]::ReadAllBytes($missing)
    Remove-Item -LiteralPath $missing
    $rejected = $false
    try { & (Join-Path $workspace 'scripts\deploy.ps1') -GamePath $fakeGame -PackageRoot $package | Out-Null } catch { $rejected = $_.ToString().Contains('Candidate artifact is missing or changed') }
    Assert-Test $rejected 'Missing late artifact rejects deployment before mutation'
    foreach ($artifact in $manifest.artifacts) {
        Assert-Test ((Get-FileHash -LiteralPath (Join-Path $fakeGame "Data\$($artifact.path)")).Hash -ceq $before[$artifact.path]) "Preflight preserves $($artifact.path)"
    }
    [System.IO.File]::WriteAllBytes($missing, $bytes)
    $global:cpoiPipelineMockState.copies = 0
    function Copy-Item {
        param([string]$LiteralPath, [string]$Destination, [switch]$Force)
        if ($Destination.StartsWith((Join-Path $global:cpoiPipelineMockState.game 'Data') + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
            $global:cpoiPipelineMockState.copies++
            if ($global:cpoiPipelineMockState.copies -eq 3) { throw 'Injected third live copy failure.' }
        }
        Microsoft.PowerShell.Management\Copy-Item -LiteralPath $LiteralPath -Destination $Destination -Force:$Force
    }
    $rejected = $false
    try { & (Join-Path $workspace 'scripts\deploy.ps1') -GamePath $fakeGame -PackageRoot $package | Out-Null } catch { Write-Host "Injected failure result: $_"; $rejected = $_.ToString().Contains('previous artifacts were restored') }
    Assert-Test $rejected 'Partial deployment reports successful rollback'
    foreach ($artifact in $manifest.artifacts) {
        Assert-Test ((Get-FileHash -LiteralPath (Join-Path $fakeGame "Data\$($artifact.path)")).Hash -ceq $before[$artifact.path]) "Rollback preserves $($artifact.path)"
    }
    Remove-Item Function:\Copy-Item
    Remove-Item Function:\Get-Item
    Remove-Item Function:\Get-Process
}
foreach ($case in @(
    @{ name = 'core-only'; modules = @('core'); count = 6 }
)) {
    $fixture = Join-Path $root $case.name
    $stage = Join-Path $fixture 'staging'
    $data = Join-Path $stage 'Data'
    [System.IO.Directory]::CreateDirectory((Join-Path $stage 'validation')) | Out-Null
    # Deliberately dirty staging: unselected optional and old UI outputs must not
    # become new dependencies or leak into a module-selected package.
    $allPaths = @(Get-ModuleArtifactPaths -Modules @('core')) + @(Get-ExcludedArtifactPaths)
    foreach ($path in $allPaths) {
        $file = Join-Path $data $path
        [System.IO.Directory]::CreateDirectory((Split-Path -Parent $file)) | Out-Null
        [System.IO.File]::WriteAllText($file, "fixture:$path")
    }
    $artifacts = foreach ($path in (Get-ModuleArtifactPaths -Modules $case.modules)) {
        [ordered]@{ path = $path; length = (Get-Item -LiteralPath (Join-Path $data $path)).Length; sha256 = (Get-FileHash -LiteralPath (Join-Path $data $path)).Hash }
    }
    $manifest = [ordered]@{
        schema = 'claimable-pois.candidate.v2'; status = 'gameplay-pending'; modules = $case.modules
        source_commit = (& git -C $workspace rev-parse HEAD).Trim(); source_identity = Get-SourceIdentity -Workspace $workspace
        artifacts = @($artifacts); gates = @(); inputs = @()
    }
    $manifest.gates = @(Get-RequiredCandidateGates -Manifest $manifest | ForEach-Object { [ordered]@{ name = $_; status = 'passed' } })
    Write-AtomicJson -Path (Join-Path $stage 'validation\candidate.json') -Value $manifest
    Write-AtomicJson -Path (Join-Path $stage 'validation\esm-validation.json') -Value ([ordered]@{
        schema = 'claimable-pois.esm-validation.v1'; status = 'passed'; filename = 'ClaimablePOIs.esm'
        validation_passed = $true; esm_sha256 = (Get-FileHash -LiteralPath (Join-Path $data 'ClaimablePOIs.esm')).Hash
    })
    $package = Join-Path $fixture 'package'
    & (Join-Path $workspace 'scripts\package.ps1') -StagingRoot $stage -OutputRoot $package
    $packagedPaths = @(Get-ChildItem -LiteralPath (Join-Path $package 'Data') -Recurse -File)
    Assert-Test ($packagedPaths.Count -eq $case.count) "$($case.name) packages only explicit modules despite stale staging"
    Assert-Test (@($packagedPaths | Where-Object FullName -Match 'OSFUI').Count -eq 0) "$($case.name) excludes removed dashboard"
    & (Join-Path $workspace 'scripts\release.ps1') -Version '0.0.0-test' -PackageRoot $package -StagingRoot $stage -DistRoot (Join-Path $fixture 'dist') | Out-Null
    $zip = [System.IO.Compression.ZipFile]::OpenRead((Join-Path $fixture 'dist\ClaimablePOIs-v0.0.0-test.zip'))
    try {
        Assert-Test ($zip.Entries.Count -eq ($case.count + 3) -and @($zip.Entries | Where-Object FullName -Match 'OSFUI').Count -eq 0) "$($case.name) release retains exact module selection"
        Assert-Test (@($zip.Entries | Where-Object { -not $_.FullName.StartsWith('Data/', [StringComparison]::Ordinal) }).Count -eq 0 -and $null -ne $zip.GetEntry('Data/Docs/ClaimablePOIs/LICENSE') -and $null -eq $zip.GetEntry('README.md')) 'Release contains only Data files with required licenses'
    } finally { $zip.Dispose() }
    $corePackage = Join-Path $fixture 'core-projection\package'
    & (Join-Path $workspace 'scripts\package.ps1') -StagingRoot $stage -OutputRoot $corePackage -CoreOnly
    $coreManifest = Get-Content -LiteralPath (Join-Path $corePackage 'candidate.json') -Raw | ConvertFrom-Json
    Assert-Test (@(Get-ChildItem -LiteralPath (Join-Path $corePackage 'Data') -Recurse -File).Count -eq 6 -and
        @($coreManifest.modules).Count -eq 1 -and $coreManifest.modules[0] -ceq 'core' -and
        $coreManifest.parent_receipt_sha256 -ceq (Get-FileHash -LiteralPath (Join-Path $stage 'validation\candidate.json')).Hash) 'Core projection preserves verified source receipt and omits all optional files'
    if ($case.name -eq 'core-only') {
        $originalGates = $manifest.gates
        $manifest.gates = @($manifest.gates | Where-Object { $_.name -ne 'generator-regressions' })
        Write-AtomicJson -Path (Join-Path $stage 'validation\candidate.json') -Value $manifest
        $rejected = $false
        try { & (Join-Path $workspace 'scripts\package.ps1') -StagingRoot $stage -OutputRoot (Join-Path $fixture 'missing-gate\package') } catch { $rejected = $_.ToString().Contains('Missing successful gate') }
        Assert-Test ($rejected -and -not (Test-Path -LiteralPath (Join-Path $fixture 'missing-gate\package'))) 'Core regression gate is required before packaging'
        $manifest.gates = $originalGates
    }
    $manifest.modules = @('core','dashboard')
    $rejected = $false
    try { Assert-ArtifactManifest -DataRoot $data -Manifest ([pscustomobject]$manifest) } catch { $rejected = $_.ToString().Contains('known, unique module names') }
    Assert-Test $rejected 'Removed dashboard cannot be selected in new receipts'
    $manifest.modules = $case.modules
    $originalArtifacts = $manifest.artifacts
    $manifest.artifacts = @($manifest.artifacts | Where-Object { $_.path -cne 'ClaimablePOIs.esm' })
    $rejected = $false
    try { Assert-ArtifactManifest -DataRoot $data -Manifest ([pscustomobject]$manifest) } catch { $rejected = $_.ToString().Contains('does not match') }
    Assert-Test $rejected 'Incomplete declared module rejected'
    $manifest.artifacts = $originalArtifacts
    if ($case.name -eq 'core-only') {
        $fakeGame = Join-Path $fixture 'game'
        [System.IO.Directory]::CreateDirectory((Join-Path $fakeGame 'Data')) | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $fakeGame 'Starfield.exe'), 'inert fixture')
        $global:cpoiPipelineMockState = @{ game = $fakeGame }
        function Get-Item {
            param([string]$LiteralPath)
            if ($LiteralPath -eq (Join-Path $global:cpoiPipelineMockState.game 'Starfield.exe')) { return [pscustomobject]@{ VersionInfo = [pscustomobject]@{ FileVersion = '1.16.244.0' } } }
            Microsoft.PowerShell.Management\Get-Item -LiteralPath $LiteralPath
        }
        function Get-Process { param([string[]]$Name, [string]$ErrorAction) }
        try {
            & (Join-Path $workspace 'scripts\deploy.ps1') -GamePath $fakeGame -PackageRoot $package | Out-Null
            Assert-Test (@(Get-ChildItem -LiteralPath (Join-Path $fakeGame 'Data') -File -Recurse).Count -eq 6) 'Core-only deployment succeeds into empty inert install'
            foreach ($stalePath in @('Scripts\CPOI_LivingOutpostsFixScript.pex', (Get-LegacyDashboardArtifactPaths)[0])) {
                $staleFile = Join-Path $fakeGame ('Data\' + $stalePath)
                [System.IO.Directory]::CreateDirectory((Split-Path -Parent $staleFile)) | Out-Null
                [System.IO.File]::WriteAllText($staleFile, 'stale optional file')
                $rejected = $false
                try { & (Join-Path $workspace 'scripts\deploy.ps1') -GamePath $fakeGame -PackageRoot $package | Out-Null } catch { $rejected = $_.ToString().Contains('Live optional groups') }
                Assert-Test $rejected 'Core-only deployment refuses stale optional/UI leftovers'
                Remove-Item -LiteralPath $staleFile
            }
        } finally {
            Remove-Item Function:\Get-Item
            Remove-Item Function:\Get-Process
        }
    }
}
foreach ($invalidModules in @(@('core','core'), @('living-outposts'), @('core','unknown'))) {
    $rejected = $false
    try { Get-ModuleArtifactPaths -Modules $invalidModules | Out-Null } catch { $rejected = $true }
    Assert-Test $rejected 'Invalid module set rejected'
}
Write-Host "Passed $passed artifact pipeline checks; inert files only. Evidence: $root"

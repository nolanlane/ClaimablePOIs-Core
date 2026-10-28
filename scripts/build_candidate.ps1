[CmdletBinding()]
param(
    [string]$GameDataPath = 'S:\SteamLibrary\steamapps\common\Starfield\Data',
    [string]$PreserveRuntimeFromPackage = '',
    [string]$StagingRoot = ''
)

. (Join-Path $PSScriptRoot 'artifacts.ps1')
$workspace = Get-CanonicalPath (Join-Path $PSScriptRoot '..')
Assert-CandidateSourceCommitted -Workspace $workspace
. (Join-Path $PSScriptRoot 'dotnet.ps1')
$dotnetSelection = Initialize-PinnedDotNet -Workspace $workspace
$sourceCommit = (& git -C $workspace rev-parse HEAD).Trim()
$identity = Get-SourceIdentity -Workspace $workspace
$evidence = Join-Path $workspace ('scratch\candidate-' + [guid]::NewGuid().ToString('N'))
Assert-NoExistingReparsePoint -Path $evidence
[System.IO.Directory]::CreateDirectory($evidence) | Out-Null
Write-AtomicJson -Path (Join-Path $evidence 'dotnet-preflight.json') -Value $dotnetSelection
$modules = @('core')
$selectedPaths = @(Get-ModuleArtifactPaths -Modules $modules)
$staging = if ([string]::IsNullOrWhiteSpace($StagingRoot)) { Join-Path $evidence 'staging' } else { Get-CanonicalPath $StagingRoot }
Assert-WorkspaceChild -Path $staging -Workspace $workspace
Assert-NoExistingReparsePoint -Path $staging
Assert-NotInsideStarfieldInstall -Path $staging
if (Test-Path -LiteralPath $staging) { throw "Refusing to reuse candidate staging: $staging" }
$receipt = Join-Path $staging 'validation\candidate.json'
Assert-NoExistingReparsePoint -Path $receipt
foreach ($path in @('Data\SFSE\Plugins', 'Data\Scripts', 'Data\Seq')) {
    Assert-NoExistingReparsePoint -Path (Join-Path $staging $path)
}
$gates = [System.Collections.Generic.List[object]]::new()
$inputNames = @('Starfield.esm')
$inputs = foreach ($name in ($inputNames | Select-Object -Unique)) {
    [ordered]@{ name = $name; sha256 = (Get-FileHash -LiteralPath (Join-Path $GameDataPath $name)).Hash }
}
$regressionInputs = @()
$toolInputs = [ordered]@{
    'Starfield.exe' = Join-Path $GameDataPath '..\Starfield.exe'
    'sfse_1_16_244.dll' = Join-Path $GameDataPath '..\sfse_1_16_244.dll'
    'AddressLibrary' = Join-Path $GameDataPath 'SFSE\Plugins\versionlib-1-16-244-0.bin'
    'Cassiopeia' = Join-Path $GameDataPath 'SFSE\Plugins\CassiopeiaPapyrusExtender.dll'
    'PapyrusCompiler' = Join-Path $GameDataPath '..\Tools\Papyrus Compiler\PapyrusCompiler.exe'
    'PapyrusFlags' = Join-Path $workspace 'tools\PapyrusSource\Starfield_Papyrus_Flags.flg'
}
$toolchain = foreach ($name in $toolInputs.Keys) {
    [ordered]@{ name = $name; sha256 = (Get-FileHash -LiteralPath $toolInputs[$name]).Hash }
}
$papyrusTrees = [ordered]@{
    'vanilla-papyrus-imports' = Join-Path $workspace 'tools\PapyrusSource\Base'
    'installed-papyrus-imports' = Join-Path $GameDataPath 'Scripts\Source'
}
function Get-PapyrusTreeIdentity {
    param([string]$Path)
    $lines = foreach ($file in (Get-ChildItem -LiteralPath $Path -Filter '*.psc' -File -Recurse | Sort-Object FullName)) {
        "$($file.FullName.Substring($Path.Length + 1))=$((Get-FileHash -LiteralPath $file.FullName).Hash)"
    }
    return [Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData([System.Text.Encoding]::UTF8.GetBytes($lines -join "`n")))
}
$importIdentities = foreach ($name in $papyrusTrees.Keys) {
    [ordered]@{ name = $name; sha256 = Get-PapyrusTreeIdentity -Path $papyrusTrees[$name] }
}
function Invoke-CandidateGate {
    param([string]$Name, [string]$Script, [string[]]$Arguments = @())
    Write-Host "Running candidate gate: $Name"
    $log = Join-Path $evidence "$Name.log"
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot $Script) @Arguments *> $log
    $code = $LASTEXITCODE
    Get-Content -LiteralPath $log -Tail 8 | Write-Host
    if ($code -ne 0) { throw "Candidate gate $Name failed ($code). Evidence: $log" }
    $gates.Add([ordered]@{ name = $Name; status = 'passed'; log_sha256 = (Get-FileHash -LiteralPath $log).Hash })
}
Push-Location $workspace
try {
    Invoke-CandidateGate 'native-tests' 'test.ps1'
    Invoke-CandidateGate 'native-build' 'build.ps1' @('-StagingRoot', $staging)
    $core = Join-Path $evidence 'core'
    Invoke-CandidateGate 'core-generation' 'build_esm.ps1' @('-OutputDir', $core)
    Invoke-CandidateGate 'core-validation' 'validate_esm.ps1' @('-InputPlugin', (Join-Path $core 'ClaimablePOIs.esm'), '-ArtifactRoot', (Join-Path $staging 'validation'))
    $data = Join-Path $staging 'Data'
    foreach ($path in @('ClaimablePOIs.esm', 'Seq\ClaimablePOIs.esm.seq')) {
        $destination = Join-Path $data $path
        Assert-NoExistingReparsePoint -Path $destination
        [System.IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
        Copy-Item -LiteralPath (Join-Path $core $path) -Destination $destination -Force
    }
    Invoke-CandidateGate 'papyrus-build' 'build_scripts.ps1' @('-CompilerPath', $toolInputs['PapyrusCompiler'], '-GameDataPath', $GameDataPath, '-StagingRoot', $staging)
    Invoke-CandidateGate 'generator-regressions' 'test_generators.ps1' @('-ClaimablePath', (Join-Path $data 'ClaimablePOIs.esm'))
    Invoke-CandidateGate 'artifact-pipeline' '..\tests\artifact_pipeline_tests.ps1'
    if ((Get-SourceIdentity -Workspace $workspace) -cne $identity) { throw 'Build inputs changed during candidate preparation.' }
    $runtimePreservation = $null
    if (-not [string]::IsNullOrWhiteSpace($PreserveRuntimeFromPackage)) {
        # Run all current gates first, then retain byte-identical DLL/PEX controls
        # only when their source, compiler/import and dependency identities agree.
        $baselinePackage = Get-CanonicalPath $PreserveRuntimeFromPackage
        Assert-WorkspaceChild -Path $baselinePackage -Workspace $workspace
        Assert-NoExistingReparsePoint -Path $baselinePackage
        Assert-NotInsideStarfieldInstall -Path $baselinePackage
        $baselineReceipt = Join-Path $baselinePackage 'candidate.json'
        $baseline = Get-Content -LiteralPath $baselineReceipt -Raw | ConvertFrom-Json
        $baselineData = Join-Path $baselinePackage 'Data'
        Assert-ArtifactManifest -DataRoot $baselineData -Manifest $baseline
        foreach ($gate in @('native-tests','native-build','papyrus-build')) {
            if (@($baseline.gates | Where-Object { $_.name -ceq $gate -and $_.status -ceq 'passed' }).Count -ne 1) {
                throw "Runtime baseline lacks gate: $gate"
            }
        }
        & git -C $workspace merge-base --is-ancestor $baseline.source_commit $sourceCommit
        if ($LASTEXITCODE -ne 0) { throw 'Runtime baseline source is not a known candidate ancestor.' }
        & git -C $workspace diff --quiet $baseline.source_commit $sourceCommit -- src patches external/CommonLibSF xmake.lua global.json DEPENDENCIES.md scripts/build.ps1 scripts/test.ps1 scripts/build_scripts.ps1 scripts/prepare_commonlib.ps1 scripts/common.ps1
        if ($LASTEXITCODE -ne 0) { throw 'Runtime source/build inputs differ from the proposed baseline.' }
        if ($baseline.runtime -cne '1.16.244.0' -or $baseline.sfse -cne '0.2.21' -or
            $baseline.commonlib_commit -cne (& git -C (Join-Path $workspace 'external\CommonLibSF') rev-parse HEAD).Trim()) {
            throw 'Runtime baseline version/dependency mismatch.'
        }
        foreach ($set in @(
            @{ current = @($toolchain); previous = @($baseline.toolchain_inputs) },
            @{ current = @($importIdentities); previous = @($baseline.papyrus_imports) }
        )) {
            if ($set.current.Count -ne $set.previous.Count) { throw 'Runtime baseline input inventory differs.' }
            foreach ($baselineInput in $set.current) {
                $old = @($set.previous | Where-Object { $_.name -ceq $baselineInput.name })
                if ($old.Count -ne 1 -or $old[0].sha256 -cne $baselineInput.sha256) { throw "Runtime baseline input differs: $($baselineInput.name)" }
            }
        }
        $retained = foreach ($path in ($selectedPaths | Where-Object { $_ -like '*.dll' -or $_ -like '*.pex' })) {
            $old = @($baseline.artifacts | Where-Object { $_.path -ceq $path })
            if ($old.Count -ne 1) { throw "Runtime baseline lacks artifact: $path" }
            $destination = Join-Path $data $path
            $rebuilt = Join-Path $evidence "rebuilt-runtime\$path"
            [System.IO.Directory]::CreateDirectory((Split-Path -Parent $rebuilt)) | Out-Null
            Copy-Item -LiteralPath $destination -Destination $rebuilt
            $rebuiltHash = (Get-FileHash -LiteralPath $destination).Hash
            if ((Get-FileHash -LiteralPath $rebuilt).Hash -cne $rebuiltHash) { throw 'Fresh runtime output backup differs.' }
            Copy-Item -LiteralPath (Join-Path $baselineData $path) -Destination $destination -Force
            if ((Get-FileHash -LiteralPath $destination).Hash -cne $old[0].sha256) { throw "Retained runtime hash differs: $path" }
            [ordered]@{ path = $path; retained_sha256 = $old[0].sha256; rebuilt_sha256 = $rebuiltHash }
        }
        Assert-ArtifactManifest -DataRoot $baselineData -Manifest $baseline
        $runtimePreservation = [ordered]@{
            source_package = $baselinePackage; source_commit = $baseline.source_commit
            receipt_sha256 = (Get-FileHash -LiteralPath $baselineReceipt).Hash; artifacts = @($retained)
        }
        $preservationLog = Join-Path $evidence 'runtime-preservation.json'
        Write-AtomicJson -Path $preservationLog -Value $runtimePreservation
        $gates.Add([ordered]@{ name = 'unchanged-runtime-preservation'; status = 'passed'; log_sha256 = (Get-FileHash -LiteralPath $preservationLog).Hash })
    }
    $artifacts = foreach ($path in $selectedPaths) {
        $file = Get-Item -LiteralPath (Join-Path $data $path)
        [ordered]@{ path = $path; length = $file.Length; sha256 = (Get-FileHash -LiteralPath $file.FullName).Hash }
    }
    foreach ($input in (@($inputs) + @($regressionInputs))) {
        if ((Get-FileHash -LiteralPath (Join-Path $GameDataPath $input.name)).Hash -cne $input.sha256) { throw "Master changed during preparation: $($input.name)" }
    }
    foreach ($input in $toolchain) {
        if ((Get-FileHash -LiteralPath $toolInputs[$input.name]).Hash -cne $input.sha256) { throw "Toolchain input changed: $($input.name)" }
    }
    foreach ($input in $importIdentities) {
        if ((Get-PapyrusTreeIdentity -Path $papyrusTrees[$input.name]) -cne $input.sha256) { throw "Papyrus imports changed: $($input.name)" }
    }
    if (@(& git -C (Join-Path $workspace 'external\CommonLibSF') status --porcelain).Count -ne 0) { throw 'CommonLibSF was not restored to a clean state.' }
    $manifest = [ordered]@{
        schema = 'claimable-pois.candidate.v2'; status = 'gameplay-pending'; modules = @($modules)
        source_commit = $sourceCommit; source_identity = $identity; built_utc = [DateTime]::UtcNow.ToString('o')
        runtime = '1.16.244.0'; sfse = '0.2.21'; dotnet_sdk = (& dotnet --version).Trim()
        commonlib_commit = (& git -C (Join-Path $workspace 'external\CommonLibSF') rev-parse HEAD).Trim()
        inputs = @($inputs); regression_inputs = @($regressionInputs); toolchain_inputs = @($toolchain); papyrus_imports = @($importIdentities)
        gates = @($gates.ToArray()); artifacts = @($artifacts)
        runtime_preservation = $runtimePreservation
    }
    Assert-ArtifactManifest -DataRoot $data -Manifest $manifest
    Write-AtomicJson -Path $receipt -Value $manifest
    Write-Host "Candidate ready for packaging: $receipt (evidence: $evidence)"
} finally { Pop-Location }

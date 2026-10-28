[CmdletBinding()]
param(
    [string] $CompilerPath = 'S:\SteamLibrary\steamapps\common\Starfield\Tools\Papyrus Compiler\PapyrusCompiler.exe',
    [string] $GameDataPath = 'S:\SteamLibrary\steamapps\common\Starfield\Data',
    [string] $StagingRoot = (Join-Path $PSScriptRoot '..\staging')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'common.ps1')
$staging = Get-CanonicalPath $StagingRoot
Assert-WorkspaceChild -Path $staging -Workspace $projectRoot
Assert-NoExistingReparsePoint -Path $staging
Assert-NotInsideStarfieldInstall -Path $staging
$papyrusSourceDir = Join-Path $PSScriptRoot 'papyrus'
$baseSourceDir = Join-Path $projectRoot 'tools\PapyrusSource\Base'
$flagsFile = Join-Path $projectRoot 'tools\PapyrusSource\Starfield_Papyrus_Flags.flg'
$stagingScriptsDir = Join-Path $staging 'Data\Scripts'
Assert-NoExistingReparsePoint -Path $stagingScriptsDir
$installedSourceDir = Join-Path $GameDataPath 'Scripts\Source'
$installedUserSourceDir = Join-Path $installedSourceDir 'User'

[System.IO.Directory]::CreateDirectory($stagingScriptsDir) | Out-Null

if (-not (Test-Path -LiteralPath $CompilerPath -PathType Leaf)) {
    throw "PapyrusCompiler not found at $CompilerPath"
}

if (-not (Test-Path -LiteralPath $baseSourceDir -PathType Container)) {
    throw "Base Papyrus source directory not found at $baseSourceDir"
}
if (-not (Test-Path -LiteralPath $papyrusSourceDir -PathType Container)) {
    throw "Project Papyrus source directory not found at $papyrusSourceDir"
}
if (-not (Test-Path -LiteralPath $flagsFile -PathType Leaf)) {
    throw "Papyrus flags file not found at $flagsFile"
}
$requiredDependencySources = @(
    (Join-Path $installedUserSourceDir 'CassiopeiaPapyrusExtender.psc')
)
foreach ($dependencySource in $requiredDependencySources) {
    if (-not (Test-Path -LiteralPath $dependencySource -PathType Leaf)) {
        throw "Required installed Papyrus dependency source not found: $dependencySource"
    }
}

$importPaths = "$papyrusSourceDir;$stagingScriptsDir;$installedSourceDir;$installedUserSourceDir;$baseSourceDir"

$scriptsToCompile = @(
    'CPOI_Native.psc',
    'CPOI_ClaimQuestScript.psc',
    'CPOI_ClaimBeaconEffect.psc'
)

Push-Location $papyrusSourceDir
try {
    foreach ($script in $scriptsToCompile) {
        Assert-NoExistingReparsePoint -Path (Join-Path $stagingScriptsDir ([System.IO.Path]::ChangeExtension($script, '.pex')))
        Write-Output "Compiling $script..."
        $output = & $CompilerPath $script "-import=$importPaths" "-output=$stagingScriptsDir" "-flags=$flagsFile" 2>&1
        $exitCode = $LASTEXITCODE
        if ($exitCode -ne 0) {
            Write-Error ($output -join "`n")
            throw "Compilation of $script failed with exit code $exitCode"
        }
        $pexName = [System.IO.Path]::ChangeExtension($script, ".pex")
        $pexPath = Join-Path $stagingScriptsDir $pexName
        if (-not (Test-Path -LiteralPath $pexPath)) {
            throw "Expected output $pexPath was not generated"
        }
        $hash = (Get-FileHash -LiteralPath $pexPath -Algorithm SHA256).Hash
        Write-Output "  compiled: $pexName ($hash)"
    }
}
finally {
    Pop-Location
}

Write-Output "Successfully compiled $($scriptsToCompile.Count) scripts."

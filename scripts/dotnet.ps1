# Shared SDK selection. Importing this file does not change the environment.
function Resolve-PinnedDotNetRoot {
    param([Parameter(Mandatory)][string]$Version, [Parameter(Mandatory)][string[]]$CandidateRoots)
    foreach ($candidate in $CandidateRoots) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $root = [IO.Path]::GetFullPath($candidate)
        if ((Test-Path -LiteralPath (Join-Path $root 'dotnet.exe') -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $root "sdk\$Version\dotnet.dll") -PathType Leaf)) { return $root }
    }
    throw "Pinned .NET SDK $Version is unavailable. Install it into .cache/dotnet-$Version or set CPOI_DOTNET_ROOT to a host containing that exact SDK. Selection failed before any build."
}

function Initialize-PinnedDotNet {
    param([Parameter(Mandatory)][string]$Workspace)
    $pin = (Get-Content -LiteralPath (Join-Path $Workspace 'global.json') -Raw | ConvertFrom-Json).sdk.version
    $roots = @()
    if (-not [string]::IsNullOrWhiteSpace($env:CPOI_DOTNET_ROOT)) {
        # Explicit override errors must not be hidden by an automatic fallback.
        $roots = @($env:CPOI_DOTNET_ROOT)
    } else {
        $roots = @((Join-Path $Workspace ".cache\dotnet-$pin"))
        if (-not [string]::IsNullOrWhiteSpace($env:DOTNET_ROOT)) { $roots += $env:DOTNET_ROOT }
        $hostCommand = Get-Command dotnet.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $hostCommand) { $roots += Split-Path -Parent $hostCommand.Source }
    }
    $root = Resolve-PinnedDotNetRoot -Version $pin -CandidateRoots $roots
    $hostPath = Join-Path $root 'dotnet.exe'
    Push-Location $Workspace
    try {
        $selected = @(& $hostPath --version 2>&1)
        if ($LASTEXITCODE -ne 0 -or $selected.Count -ne 1 -or $selected[0].ToString().Trim() -cne $pin) {
            throw "Pinned .NET host failed SDK $pin verification before any build: $($selected -join ' ')"
        }
    } finally { Pop-Location }
    # Process-only selection is inherited by the child gate processes. No user
    # PATH, global installation or global.json pin is modified.
    $env:DOTNET_ROOT = $root
    $env:PATH = $root + [IO.Path]::PathSeparator + $env:PATH
    return [ordered]@{ sdk = $pin; root = $root; host_sha256 = (Get-FileHash -LiteralPath $hostPath).Hash }
}

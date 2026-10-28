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

function Test-SameOrChildPath {
    param(
        [Parameter(Mandatory)][string]$Candidate,
        [Parameter(Mandatory)][string]$Parent
    )

    if ($Candidate.Equals($Parent, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }

    $prefix = $Parent + [System.IO.Path]::DirectorySeparatorChar
    return $Candidate.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-WorkspaceChild {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Workspace
    )

    if ($Path.Equals($Workspace, [System.StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-SameOrChildPath -Candidate $Path -Parent $Workspace)) {
        throw "Path must remain a child of the workspace: $Path"
    }
}

function Assert-NoExistingReparsePoint {
    param([Parameter(Mandatory)][string]$Path)

    $cursor = $Path
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing reparse-point path: $cursor"
            }
        }

        $parent = [System.IO.Directory]::GetParent($cursor)
        if ($null -eq $parent) {
            break
        }
        $cursor = $parent.FullName
    }
}

function Assert-NotInsideStarfieldInstall {
    param([Parameter(Mandatory)][string]$Path)

    $cursor = $Path
    while ($cursor) {
        if (Test-Path -LiteralPath (Join-Path $cursor 'Starfield.exe') -PathType Leaf) {
            throw "Path must not be inside a live Starfield installation: $Path"
        }

        $parent = [System.IO.Directory]::GetParent($cursor)
        if ($null -eq $parent) {
            break
        }
        $cursor = $parent.FullName
    }
}

function Write-AtomicJson {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][object]$Value
    )

    $temporaryPath = $Path + '.tmp'
    $json = $Value | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText(
        $temporaryPath,
        $json + [Environment]::NewLine,
        [System.Text.UTF8Encoding]::new($false)
    )
    [System.IO.File]::Move($temporaryPath, $Path, $true)
}

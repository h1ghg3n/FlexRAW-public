#requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$StagingDirectory,

    [Parameter(Mandatory)]
    [string]$ProfileDirectory,

    [ValidateRange(1, 60)]
    [int]$ObservationSeconds = 8
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$stagingRoot = [IO.Path]::GetFullPath($StagingDirectory)
$profileRoot = [IO.Path]::GetFullPath($ProfileDirectory)
if (-not (Test-Path -LiteralPath $stagingRoot -PathType Container)) {
    throw "Desktop staging directory does not exist: $stagingRoot"
}
if (Test-Path -LiteralPath $profileRoot) {
    throw "Desktop smoke profile directory already exists: $profileRoot"
}

$entryPoint = Join-Path $stagingRoot 'flexraw.exe'
$platformPlugin = Join-Path $stagingRoot 'plugins\platforms\qwindows.dll'
$manifestPath = Join-Path $stagingRoot 'artifact-manifest.sha256'
foreach ($requiredPath in @($entryPoint, $platformPlugin, $manifestPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required staged Desktop artifact is missing: $requiredPath"
    }
}

$stagingPrefix = $stagingRoot.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) +
    [IO.Path]::DirectorySeparatorChar
$manifestLines = @(Get-Content -LiteralPath $manifestPath)
if ($manifestLines.Count -eq 0) {
    throw 'Desktop staging artifact manifest is empty.'
}

foreach ($line in $manifestLines) {
    if ($line -notmatch '^(?<Hash>[0-9a-f]{64})  (?<RelativePath>.+)$') {
        throw "Invalid Desktop artifact manifest line: $line"
    }

    $relativePath = $Matches.RelativePath.Replace('/', [IO.Path]::DirectorySeparatorChar)
    if ([IO.Path]::IsPathRooted($relativePath)) {
        throw "Desktop artifact manifest contains a rooted path: $relativePath"
    }

    $artifactPath = [IO.Path]::GetFullPath((Join-Path $stagingRoot $relativePath))
    if (-not $artifactPath.StartsWith($stagingPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Desktop artifact manifest path escapes staging: $relativePath"
    }
    if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
        throw "Desktop artifact manifest entry is missing: $relativePath"
    }

    $actualHash = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $Matches.Hash) {
        throw "Desktop artifact hash mismatch: $relativePath"
    }
}

$roamingProfile = Join-Path $profileRoot 'AppData\Roaming'
$localProfile = Join-Path $profileRoot 'AppData\Local'
New-Item -ItemType Directory -Path $roamingProfile, $localProfile | Out-Null
$standardOutputPath = Join-Path $profileRoot 'flexraw.stdout.log'
$standardErrorPath = Join-Path $profileRoot 'flexraw.stderr.log'
$env:APPDATA = $roamingProfile
$env:LOCALAPPDATA = $localProfile
$env:QT_PLUGIN_PATH = ''

$process = $null
try {
    $process = Start-Process -FilePath $entryPoint `
        -WorkingDirectory $stagingRoot `
        -RedirectStandardOutput $standardOutputPath `
        -RedirectStandardError $standardErrorPath `
        -PassThru
    Start-Sleep -Seconds $ObservationSeconds
    $process.Refresh()
    if ($process.HasExited) {
        $standardError = if (Test-Path -LiteralPath $standardErrorPath) {
            (Get-Content -LiteralPath $standardErrorPath -Raw).Trim()
        }
        else {
            ''
        }
        throw "Staged Desktop exited during startup observation (exit=$($process.ExitCode)): $standardError"
    }

    Write-Output "Staged Desktop remained alive for $ObservationSeconds seconds on an isolated profile."
}
finally {
    if ($null -ne $process) {
        $process.Refresh()
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        if (-not $process.WaitForExit(10000)) {
            throw "Staged Desktop process did not terminate after the smoke observation: $($process.Id)"
        }
        $process.Dispose()
    }
}

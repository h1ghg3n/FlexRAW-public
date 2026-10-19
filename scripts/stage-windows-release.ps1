#requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Desktop', 'Mcp', 'Worker', 'Cli')]
    [string]$Profile,

    [Parameter(Mandatory)]
    [string]$SourceDirectory,

    [Parameter(Mandatory)]
    [string]$DestinationDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 목적: 비교와 출력에 사용할 canonical absolute path 반환
# 입력: Path - 상대 또는 절대 filesystem path
# 출력: 정규화된 absolute path
function Get-CanonicalPath {
    param([string]$Path)

    return [IO.Path]::GetFullPath($Path)
}

# 목적: 선택한 runtime file을 staging의 동일한 상대 위치로 복사
# 입력: SourceRoot, DestinationRoot, RelativePath - source/staging root와 복사할 상대 경로
# 출력: 없음
function Copy-StagedFile {
    param(
        [string]$SourceRoot,
        [string]$DestinationRoot,
        [string]$RelativePath
    )

    $sourcePath = Join-Path $SourceRoot $RelativePath
    $destinationPath = Join-Path $DestinationRoot $RelativePath
    $destinationParent = Split-Path -Parent $destinationPath
    if (-not (Test-Path -LiteralPath $destinationParent)) {
        New-Item -ItemType Directory -Path $destinationParent | Out-Null
    }

    Copy-Item -LiteralPath $sourcePath -Destination $destinationPath
}

# 목적: staging file의 상대 경로와 SHA-256을 deterministic manifest로 기록
# 입력: DestinationRoot - 완성된 create-only staging root
# 출력: 생성한 manifest absolute path
function Write-ArtifactManifest {
    param([string]$DestinationRoot)

    $manifestName = 'artifact-manifest.sha256'
    $manifestPath = Join-Path $DestinationRoot $manifestName
    $lines = Get-ChildItem -LiteralPath $DestinationRoot -Recurse -File |
        Where-Object { $_.Name -ne $manifestName } |
        ForEach-Object {
            $relativePath = [IO.Path]::GetRelativePath($DestinationRoot, $_.FullName).Replace('\', '/')
            $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            [PSCustomObject]@{
                RelativePath = $relativePath
                Line = "$hash  $relativePath"
            }
        } |
        Sort-Object -Property RelativePath |
        Select-Object -ExpandProperty Line

    Set-Content -LiteralPath $manifestPath -Value $lines -Encoding utf8NoBOM
    return $manifestPath
}

$entryPoints = @{
    Desktop = 'flexraw.exe'
    Mcp = 'flexraw-mcp.exe'
    Worker = 'flexraw-worker.exe'
    Cli = 'flexraw-cli.exe'
}

$excludedRootRuntimeDlls = @(
    'd3dcompiler_47.dll',
    'icuuc.dll',
    'dxcompiler.dll',
    'dxil.dll'
)

$sourceRoot = Get-CanonicalPath -Path $SourceDirectory
$destinationRoot = Get-CanonicalPath -Path $DestinationDirectory
if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) {
    throw "Release runtime source directory does not exist: $sourceRoot"
}
if (Test-Path -LiteralPath $destinationRoot) {
    throw "Release staging destination already exists: $destinationRoot"
}

$sourcePrefix = $sourceRoot.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) +
    [IO.Path]::DirectorySeparatorChar
if ($destinationRoot.StartsWith($sourcePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Release staging destination must not be inside the runtime source directory.'
}

$entryPoint = $entryPoints[$Profile]
if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot $entryPoint) -PathType Leaf)) {
    throw "Required $Profile entry point is missing: $entryPoint"
}

New-Item -ItemType Directory -Path $destinationRoot | Out-Null
try {
    Copy-StagedFile -SourceRoot $sourceRoot -DestinationRoot $destinationRoot -RelativePath $entryPoint

    Get-ChildItem -LiteralPath $sourceRoot -File -Filter '*.dll' |
        Where-Object {
            $_.Name -notmatch '^(gtest|gmock)' -and
            $_.Name -notin $excludedRootRuntimeDlls
        } |
        Sort-Object -Property Name |
        ForEach-Object {
            Copy-StagedFile -SourceRoot $sourceRoot -DestinationRoot $destinationRoot -RelativePath $_.Name
        }

    $qtConfiguration = Join-Path $sourceRoot 'qt.conf'
    if (Test-Path -LiteralPath $qtConfiguration -PathType Leaf) {
        Copy-StagedFile -SourceRoot $sourceRoot -DestinationRoot $destinationRoot -RelativePath 'qt.conf'
    }

    $pluginRoot = Join-Path $sourceRoot 'plugins'
    if (Test-Path -LiteralPath $pluginRoot -PathType Container) {
        Get-ChildItem -LiteralPath $pluginRoot -Recurse -File -Filter '*.dll' |
            ForEach-Object { [IO.Path]::GetRelativePath($sourceRoot, $_.FullName) } |
            Sort-Object |
            ForEach-Object {
                Copy-StagedFile -SourceRoot $sourceRoot -DestinationRoot $destinationRoot -RelativePath $_
            }
    }

    $manifestPath = Write-ArtifactManifest -DestinationRoot $destinationRoot
    $artifactCount = @(Get-ChildItem -LiteralPath $destinationRoot -Recurse -File).Count
    Write-Output "Staged $Profile release package: $destinationRoot"
    Write-Output "Artifacts including manifest: $artifactCount"
    Write-Output "Manifest: $manifestPath"
}
catch {
    Remove-Item -LiteralPath $destinationRoot -Recurse -Force -ErrorAction SilentlyContinue
    throw
}

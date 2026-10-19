#requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$RuntimeStagingDirectory,

    [Parameter(Mandatory)]
    [string]$VcpkgInstalledDirectory,

    [Parameter(Mandatory)]
    [string]$LegalDirectory,

    [Parameter(Mandatory)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version,

    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9a-fA-F]{40}$')]
    [string]$SourceRevision,

    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9a-fA-F]{40}$')]
    [string]$VcpkgBaseline,

    [Parameter(Mandatory)]
    [string]$DestinationDirectory,

    [string]$Triplet = 'x64-windows'
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

# 목적: vcpkg status의 설치 package와 version을 release notice 후보로 복원
# 입력: StatusPath - vcpkg status file
# 출력: feature paragraph를 제외한 installed package object 목록
function Read-VcpkgPackages {
    param([string]$StatusPath)

    $packages = [Collections.Generic.List[object]]::new()
    $paragraphs = (Get-Content -LiteralPath $StatusPath -Raw) -split '(?:\r?\n){2,}'
    foreach ($paragraph in $paragraphs) {
        $fields = @{}
        foreach ($line in ($paragraph -split '\r?\n')) {
            if ($line -match '^(?<Name>[^:]+):\s*(?<Value>.*)$') {
                $fields[$Matches.Name] = $Matches.Value
            }
        }

        if ($fields.ContainsKey('Package') -and
            -not $fields.ContainsKey('Feature') -and
            $fields['Status'] -eq 'install ok installed') {
            $packages.Add([PSCustomObject]@{
                    Name = [string]$fields['Package']
                    Version = [string]$fields['Version']
                })
        }
    }

    return $packages
}

# 목적: vcpkg package file list에서 staged DLL 상대경로와 owning port 대응 생성
# 입력: InstalledRoot, PackageName, TripletName - vcpkg root와 package identity
# 출력: stage 상대경로와 package 이름 pair 목록
function Get-PackageRuntimeOwnership {
    param(
        [string]$InstalledRoot,
        [string]$PackageName,
        [string]$TripletName
    )

    $infoRoot = Join-Path $InstalledRoot 'vcpkg\info'
    $listFiles = @(Get-ChildItem -LiteralPath $infoRoot -File -Filter "$($PackageName)_*_$TripletName.list")
    if ($listFiles.Count -ne 1) {
        throw "Expected one vcpkg file list for $PackageName/$TripletName, found $($listFiles.Count)."
    }

    $ownership = [Collections.Generic.List[object]]::new()
    foreach ($line in (Get-Content -LiteralPath $listFiles[0].FullName)) {
        $packagePath = $line.Replace('/', '\')
        $relativePath = $null
        if ($packagePath -match '^[^\\]+\\bin\\(?<Path>[^\\]+)$') {
            $relativePath = $Matches.Path
        }
        elseif ($packagePath -match '^[^\\]+\\Qt6\\plugins\\(?<Path>.+)$') {
            $relativePath = Join-Path 'plugins' $Matches.Path
        }

        if ($null -ne $relativePath -and $relativePath.EndsWith('.dll', [StringComparison]::OrdinalIgnoreCase)) {
            $ownership.Add([PSCustomObject]@{
                    RelativePath = $relativePath
                    Package = $PackageName
                })
        }
    }

    return $ownership
}

# 목적: destination 전체 file의 상대 경로와 SHA-256을 deterministic manifest로 기록
# 입력: DestinationRoot - 완성된 public release root
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

$runtimeRoot = Get-CanonicalPath -Path $RuntimeStagingDirectory
$installedRoot = Get-CanonicalPath -Path $VcpkgInstalledDirectory
$legalRoot = Get-CanonicalPath -Path $LegalDirectory
$destinationRoot = Get-CanonicalPath -Path $DestinationDirectory

foreach ($requiredDirectory in @($runtimeRoot, $installedRoot, $legalRoot)) {
    if (-not (Test-Path -LiteralPath $requiredDirectory -PathType Container)) {
        throw "Required public release input directory does not exist: $requiredDirectory"
    }
}
if (Test-Path -LiteralPath $destinationRoot) {
    throw "Public release destination already exists: $destinationRoot"
}

$requiredLegalFiles = @('LICENSE', 'THIRD_PARTY_LICENSES.md', 'source-offer-or-link.txt')
foreach ($fileName in $requiredLegalFiles) {
    $legalPath = Join-Path $legalRoot $fileName
    if (-not (Test-Path -LiteralPath $legalPath -PathType Leaf) -or
        (Get-Item -LiteralPath $legalPath).Length -eq 0) {
        throw "Required public release legal file is missing or empty: $fileName"
    }
}

$statusPath = Join-Path $installedRoot 'vcpkg\status'
if (-not (Test-Path -LiteralPath $statusPath -PathType Leaf)) {
    throw "vcpkg status file is missing: $statusPath"
}

$excludedBuildPackages = @(
    'gtest',
    'vcpkg-cmake',
    'vcpkg-cmake-config',
    'vcpkg-cmake-get-vars',
    'vcpkg-tool-meson'
)
$packages = @(Read-VcpkgPackages -StatusPath $statusPath |
        Where-Object { $_.Name -notin $excludedBuildPackages } |
        Sort-Object -Property Name)
if ($packages.Count -eq 0) {
    throw 'No runtime vcpkg packages were found for the public release notice bundle.'
}

$ownershipByPath = [Collections.Generic.Dictionary[string, Collections.Generic.List[string]]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($package in $packages) {
    foreach ($ownership in (Get-PackageRuntimeOwnership -InstalledRoot $installedRoot `
            -PackageName $package.Name -TripletName $Triplet)) {
        if (-not $ownershipByPath.ContainsKey($ownership.RelativePath)) {
            $ownershipByPath[$ownership.RelativePath] = [Collections.Generic.List[string]]::new()
        }
        if (-not $ownershipByPath[$ownership.RelativePath].Contains($ownership.Package)) {
            $ownershipByPath[$ownership.RelativePath].Add($ownership.Package)
        }
    }
}

$runtimeDlls = @(Get-ChildItem -LiteralPath $runtimeRoot -Recurse -File -Filter '*.dll' | Sort-Object FullName)
$artifactOwners = [Collections.Generic.List[object]]::new()
foreach ($runtimeDll in $runtimeDlls) {
    $relativePath = [IO.Path]::GetRelativePath($runtimeRoot, $runtimeDll.FullName)
    if (-not $ownershipByPath.ContainsKey($relativePath)) {
        throw "Staged runtime DLL has no vcpkg package owner: $relativePath"
    }
    $artifactOwners.Add([PSCustomObject]@{
            Artifact = $relativePath.Replace('\', '/')
            Packages = ($ownershipByPath[$relativePath] | Sort-Object) -join ','
        })
}

New-Item -ItemType Directory -Path $destinationRoot | Out-Null
try {
    Get-ChildItem -LiteralPath $runtimeRoot -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $destinationRoot -Recurse
    }

    foreach ($fileName in $requiredLegalFiles) {
        Copy-Item -LiteralPath (Join-Path $legalRoot $fileName) -Destination (Join-Path $destinationRoot $fileName)
    }

    $noticeRoot = Join-Path $destinationRoot 'licenses\vcpkg'
    New-Item -ItemType Directory -Path $noticeRoot | Out-Null
    $noticeManifest = [Collections.Generic.List[object]]::new()
    foreach ($package in $packages) {
        $packageShare = Join-Path $installedRoot "$Triplet\share\$($package.Name)"
        $copyrightPath = Join-Path $packageShare 'copyright'
        if (-not (Test-Path -LiteralPath $copyrightPath -PathType Leaf)) {
            throw "Runtime package copyright file is missing: $($package.Name)"
        }

        $packageNoticeRoot = Join-Path $noticeRoot $package.Name
        New-Item -ItemType Directory -Path $packageNoticeRoot | Out-Null
        Copy-Item -LiteralPath $copyrightPath -Destination (Join-Path $packageNoticeRoot 'copyright.txt')
        $spdxSource = Join-Path $packageShare 'vcpkg.spdx.json'
        $hasSpdx = Test-Path -LiteralPath $spdxSource -PathType Leaf
        if ($hasSpdx) {
            Copy-Item -LiteralPath $spdxSource -Destination (Join-Path $packageNoticeRoot 'vcpkg.spdx.json')
        }
        $noticeManifest.Add([PSCustomObject]@{
                Package = $package.Name
                Version = $package.Version
                Copyright = "licenses/vcpkg/$($package.Name)/copyright.txt"
                Spdx = if ($hasSpdx) { "licenses/vcpkg/$($package.Name)/vcpkg.spdx.json" } else { '' }
            })
    }

    $artifactOwners |
        ForEach-Object { "$($_.Artifact)`t$($_.Packages)" } |
        Set-Content -LiteralPath (Join-Path $destinationRoot 'artifact-owners.tsv') -Encoding utf8NoBOM
    $noticeManifest |
        ForEach-Object { "$($_.Package)`t$($_.Version)`t$($_.Copyright)`t$($_.Spdx)" } |
        Set-Content -LiteralPath (Join-Path $destinationRoot 'notice-manifest.tsv') -Encoding utf8NoBOM

    [ordered]@{
        profile = 'public-agpl'
        version = $Version
        sourceRevision = $SourceRevision.ToLowerInvariant()
        vcpkgBaseline = $VcpkgBaseline.ToLowerInvariant()
        triplet = $Triplet
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destinationRoot 'release-metadata.json') `
        -Encoding utf8NoBOM

    $manifestPath = Write-ArtifactManifest -DestinationRoot $destinationRoot
    Write-Output "Finalized public Windows release package: $destinationRoot"
    Write-Output "Runtime DLLs mapped to vcpkg packages: $($runtimeDlls.Count)"
    Write-Output "Runtime package notices collected: $($packages.Count)"
    Write-Output "Manifest: $manifestPath"
}
catch {
    Remove-Item -LiteralPath $destinationRoot -Recurse -Force -ErrorAction SilentlyContinue
    throw
}

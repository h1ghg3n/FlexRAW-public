#requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$ScriptPath,

    [Parameter(Mandatory)]
    [string]$WorkingRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 목적: 조건이 거짓일 때 focused public release finalization test를 실패 처리
# 입력: Condition, Message - 검증 결과와 실패 설명
# 출력: 없음
function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

# 목적: 지정한 script block이 예외를 발생시키는지 검증
# 입력: Action, Message - 실행할 동작과 실패 설명
# 출력: 없음
function Assert-Throws {
    param(
        [scriptblock]$Action,
        [string]$Message
    )

    try {
        & $Action
    }
    catch {
        return
    }

    throw $Message
}

# 목적: synthetic fixture file을 필요한 parent directory와 함께 생성
# 입력: Root, RelativePath, Content - fixture root와 file 정보
# 출력: 없음
function New-FixtureFile {
    param(
        [string]$Root,
        [string]$RelativePath,
        [string]$Content
    )

    $path = Join-Path $Root $RelativePath
    $parent = Split-Path -Parent $path
    if (-not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent | Out-Null
    }
    Set-Content -LiteralPath $path -Value $Content -Encoding utf8NoBOM
}

$testRoot = Join-Path ([IO.Path]::GetFullPath($WorkingRoot)) ([Guid]::NewGuid().ToString('N'))
$runtimeRoot = Join-Path $testRoot 'runtime'
$installedRoot = Join-Path $testRoot 'installed'
$legalRoot = Join-Path $testRoot 'legal'
$releaseRoot = Join-Path $testRoot 'release'
$revision = '0123456789abcdef0123456789abcdef01234567'
$baseline = '89abcdef0123456789abcdef0123456789abcdef'

try {
    New-FixtureFile -Root $runtimeRoot -RelativePath 'flexraw.exe' -Content 'desktop'
    New-FixtureFile -Root $runtimeRoot -RelativePath 'Qt6Core.dll' -Content 'qt'
    New-FixtureFile -Root $runtimeRoot -RelativePath 'plugins\platforms\qwindows.dll' -Content 'plugin'
    New-FixtureFile -Root $runtimeRoot -RelativePath 'artifact-manifest.sha256' -Content 'runtime manifest'

    $status = @'
Package: qtbase
Version: 6.10.2
Architecture: x64-windows
Status: install ok installed

Package: qtbase
Feature: gui
Architecture: x64-windows
Status: install ok installed

Package: gtest
Version: 1.17.0
Architecture: x64-windows
Status: install ok installed

Package: vcpkg-cmake
Version: 2024-04-23
Architecture: x64-windows
Status: install ok installed
'@
    New-FixtureFile -Root $installedRoot -RelativePath 'vcpkg\status' -Content $status
    New-FixtureFile -Root $installedRoot -RelativePath 'vcpkg\info\qtbase_6.10.2_x64-windows.list' `
        -Content "x64-windows/bin/Qt6Core.dll`nx64-windows/Qt6/plugins/platforms/qwindows.dll"
    New-FixtureFile -Root $installedRoot -RelativePath 'vcpkg\info\gtest_1.17.0_x64-windows.list' `
        -Content 'x64-windows/bin/gtest.dll'
    New-FixtureFile -Root $installedRoot -RelativePath 'vcpkg\info\vcpkg-cmake_2024-04-23_x64-windows.list' `
        -Content 'x64-windows/share/vcpkg-cmake/copyright'
    New-FixtureFile -Root $installedRoot -RelativePath 'x64-windows\share\qtbase\copyright' -Content 'Qt notice'
    New-FixtureFile -Root $installedRoot -RelativePath 'x64-windows\share\qtbase\vcpkg.spdx.json' `
        -Content '{"name":"qtbase"}'

    New-FixtureFile -Root $legalRoot -RelativePath 'LICENSE' -Content 'AGPL test fixture'
    New-FixtureFile -Root $legalRoot -RelativePath 'THIRD_PARTY_LICENSES.md' -Content 'inventory'
    New-FixtureFile -Root $legalRoot -RelativePath 'source-offer-or-link.txt' -Content 'source link'

    & $ScriptPath -RuntimeStagingDirectory $runtimeRoot -VcpkgInstalledDirectory $installedRoot `
        -LegalDirectory $legalRoot -Version '0.2.0' -SourceRevision $revision -VcpkgBaseline $baseline `
        -DestinationDirectory $releaseRoot | Out-Null

    foreach ($requiredPath in @(
            'flexraw.exe',
            'Qt6Core.dll',
            'plugins\platforms\qwindows.dll',
            'LICENSE',
            'THIRD_PARTY_LICENSES.md',
            'source-offer-or-link.txt',
            'licenses\vcpkg\qtbase\copyright.txt',
            'licenses\vcpkg\qtbase\vcpkg.spdx.json',
            'artifact-owners.tsv',
            'notice-manifest.tsv',
            'release-metadata.json',
            'artifact-manifest.sha256')) {
        Assert-True -Condition (Test-Path -LiteralPath (Join-Path $releaseRoot $requiredPath) -PathType Leaf) `
            -Message "Final public package is missing: $requiredPath"
    }
    Assert-True -Condition (-not (Test-Path -LiteralPath (Join-Path $releaseRoot 'licenses\vcpkg\gtest'))) `
        -Message 'Test-only GTest notice leaked into the runtime notice bundle.'
    Assert-True -Condition ((Get-Content -LiteralPath (Join-Path $releaseRoot 'artifact-owners.tsv')).Count -eq 2) `
        -Message 'Every staged DLL must have one ownership record.'

    $metadata = Get-Content -LiteralPath (Join-Path $releaseRoot 'release-metadata.json') -Raw | ConvertFrom-Json
    Assert-True -Condition ($metadata.profile -eq 'public-agpl' -and $metadata.version -eq '0.2.0') `
        -Message 'Public release metadata is invalid.'
    Assert-True -Condition ($metadata.sourceRevision -eq $revision -and $metadata.vcpkgBaseline -eq $baseline) `
        -Message 'Public release provenance is invalid.'

    Assert-Throws -Action {
        & $ScriptPath -RuntimeStagingDirectory $runtimeRoot -VcpkgInstalledDirectory $installedRoot `
            -LegalDirectory $legalRoot -Version '0.2.0' -SourceRevision $revision -VcpkgBaseline $baseline `
            -DestinationDirectory $releaseRoot | Out-Null
    } -Message 'Existing public release destination must fail closed.'

    $missingLegalRoot = Join-Path $testRoot 'missing-legal'
    New-FixtureFile -Root $missingLegalRoot -RelativePath 'LICENSE' -Content 'license only'
    Assert-Throws -Action {
        & $ScriptPath -RuntimeStagingDirectory $runtimeRoot -VcpkgInstalledDirectory $installedRoot `
            -LegalDirectory $missingLegalRoot -Version '0.2.0' -SourceRevision $revision -VcpkgBaseline $baseline `
            -DestinationDirectory (Join-Path $testRoot 'missing-legal-release') | Out-Null
    } -Message 'Incomplete public legal input must fail closed.'

    $unmappedRuntimeRoot = Join-Path $testRoot 'unmapped-runtime'
    Copy-Item -LiteralPath $runtimeRoot -Destination $unmappedRuntimeRoot -Recurse
    New-FixtureFile -Root $unmappedRuntimeRoot -RelativePath 'unknown.dll' -Content 'unknown'
    $unmappedReleaseRoot = Join-Path $testRoot 'unmapped-release'
    Assert-Throws -Action {
        & $ScriptPath -RuntimeStagingDirectory $unmappedRuntimeRoot -VcpkgInstalledDirectory $installedRoot `
            -LegalDirectory $legalRoot -Version '0.2.0' -SourceRevision $revision -VcpkgBaseline $baseline `
            -DestinationDirectory $unmappedReleaseRoot | Out-Null
    } -Message 'Unowned runtime DLL must fail closed.'
    Assert-True -Condition (-not (Test-Path -LiteralPath $unmappedReleaseRoot)) `
        -Message 'Failed public release finalization must not leave a partial destination.'

    Write-Output 'Windows public release finalization script tests passed.'
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}

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

# 목적: 조건이 거짓일 때 focused staging test를 실패 처리
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

# 목적: synthetic runtime file을 필요한 parent directory와 함께 생성
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
$sourceRoot = Join-Path $testRoot 'runtime'
$stageRoot = Join-Path $testRoot 'stage'
New-Item -ItemType Directory -Path $sourceRoot | Out-Null

try {
    New-FixtureFile -Root $sourceRoot -RelativePath 'flexraw.exe' -Content 'desktop'
    New-FixtureFile -Root $sourceRoot -RelativePath 'Qt6Core.dll' -Content 'runtime'
    New-FixtureFile -Root $sourceRoot -RelativePath 'gtest.dll' -Content 'test-only'
    New-FixtureFile -Root $sourceRoot -RelativePath 'gtest_main.dll' -Content 'test-only'
    New-FixtureFile -Root $sourceRoot -RelativePath 'd3dcompiler_47.dll' -Content 'system'
    New-FixtureFile -Root $sourceRoot -RelativePath 'dxcompiler.dll' -Content 'system'
    New-FixtureFile -Root $sourceRoot -RelativePath 'dxil.dll' -Content 'system'
    New-FixtureFile -Root $sourceRoot -RelativePath 'icuuc.dll' -Content 'system'
    New-FixtureFile -Root $sourceRoot -RelativePath 'flexraw_ui_tests.exe' -Content 'test-only'
    New-FixtureFile -Root $sourceRoot -RelativePath 'acceptance.jpg' -Content 'artifact'
    New-FixtureFile -Root $sourceRoot -RelativePath 'worker.stderr.log' -Content 'log'
    New-FixtureFile -Root $sourceRoot -RelativePath 'qt.conf' -Content 'qt'
    New-FixtureFile -Root $sourceRoot -RelativePath 'plugins\platforms\qwindows.dll' -Content 'plugin'
    New-FixtureFile -Root $sourceRoot -RelativePath 'plugins\platforms\notes.txt' -Content 'not-runtime'

    & $ScriptPath -Profile Desktop -SourceDirectory $sourceRoot -DestinationDirectory $stageRoot | Out-Null

    Assert-True -Condition (Test-Path -LiteralPath (Join-Path $stageRoot 'flexraw.exe')) `
        -Message 'Desktop entry point was not staged.'
    Assert-True -Condition (Test-Path -LiteralPath (Join-Path $stageRoot 'Qt6Core.dll')) `
        -Message 'Root runtime DLL was not staged.'
    Assert-True -Condition (Test-Path -LiteralPath (Join-Path $stageRoot 'plugins\platforms\qwindows.dll')) `
        -Message 'Qt plugin DLL was not staged.'
    Assert-True -Condition (-not (Test-Path -LiteralPath (Join-Path $stageRoot 'gtest.dll'))) `
        -Message 'GTest DLL leaked into staging.'
    Assert-True -Condition (-not (Test-Path -LiteralPath (Join-Path $stageRoot 'flexraw_ui_tests.exe'))) `
        -Message 'Test executable leaked into staging.'
    foreach ($systemDll in @('d3dcompiler_47.dll', 'dxcompiler.dll', 'dxil.dll', 'icuuc.dll')) {
        Assert-True -Condition (-not (Test-Path -LiteralPath (Join-Path $stageRoot $systemDll))) `
            -Message "OS-provided runtime leaked into staging: $systemDll"
    }
    Assert-True -Condition (-not (Test-Path -LiteralPath (Join-Path $stageRoot 'acceptance.jpg'))) `
        -Message 'Acceptance artifact leaked into staging.'
    Assert-True -Condition (-not (Test-Path -LiteralPath (Join-Path $stageRoot 'worker.stderr.log'))) `
        -Message 'Log file leaked into staging.'
    Assert-True -Condition (-not (Test-Path -LiteralPath (Join-Path $stageRoot 'plugins\platforms\notes.txt'))) `
        -Message 'Non-runtime plugin file leaked into staging.'

    $manifestPath = Join-Path $stageRoot 'artifact-manifest.sha256'
    $manifestLines = @(Get-Content -LiteralPath $manifestPath)
    Assert-True -Condition ($manifestLines.Count -eq 4) -Message 'Manifest must contain exactly four staged runtime files.'
    $manifestRelativePaths = @($manifestLines | ForEach-Object { ($_ -split '  ', 2)[1] })
    $expectedRelativePaths = @('flexraw.exe', 'plugins/platforms/qwindows.dll', 'qt.conf', 'Qt6Core.dll') |
        Sort-Object
    Assert-True -Condition (($manifestRelativePaths -join "`n") -eq ($expectedRelativePaths -join "`n")) `
        -Message 'Manifest paths are missing or not deterministically ordered.'
    Assert-True -Condition (-not ($manifestLines -match 'artifact-manifest\.sha256')) `
        -Message 'Manifest must not hash itself.'

    Assert-Throws -Action {
        & $ScriptPath -Profile Desktop -SourceDirectory $sourceRoot -DestinationDirectory $stageRoot | Out-Null
    } -Message 'Existing destination must fail closed.'

    $missingEntrySource = Join-Path $testRoot 'missing-entry'
    New-Item -ItemType Directory -Path $missingEntrySource | Out-Null
    Assert-Throws -Action {
        & $ScriptPath -Profile Worker -SourceDirectory $missingEntrySource `
            -DestinationDirectory (Join-Path $testRoot 'missing-entry-stage') | Out-Null
    } -Message 'Missing profile entry point must fail closed.'

    Write-Output 'Windows release staging script tests passed.'
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}

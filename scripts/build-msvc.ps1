#requires -Version 7.0

[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateRange(1, 64)]
    [int]$Jobs = [Math]::Min(8, [Math]::Max(1, [Environment]::ProcessorCount - 2)),

    [switch]$SkipConfigure,
    [switch]$Fresh,
    [switch]$Test,
    [switch]$Benchmarks,
    [switch]$Worker,
    [switch]$Mcp,
    [switch]$Portable,
    [switch]$EnvironmentCheckOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 목적: 대소문자만 다른 Path entry를 하나의 canonical Path 값으로 병합
# 입력: Environment - 현재 process environment dictionary
# 출력: 중복 segment가 제거된 Path 문자열
function Get-CanonicalPathValue {
    param([System.Collections.IDictionary]$Environment)

    $pathEntries = @($Environment.GetEnumerator() | Where-Object {
            [string]::Equals([string]$_.Key, 'Path', [StringComparison]::OrdinalIgnoreCase)
        })
    if ($pathEntries.Count -eq 0) {
        throw 'The current process does not contain a Path environment variable.'
    }

    $orderedEntries = $pathEntries | Sort-Object `
        @{ Expression = { ([string]$_.Value).Length }; Descending = $true }, `
        @{ Expression = { [string]$_.Key }; Descending = $false }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $segments = [Collections.Generic.List[string]]::new()

    foreach ($entry in $orderedEntries) {
        foreach ($segment in ([string]$entry.Value -split ';')) {
            $candidate = $segment.Trim()
            if ($candidate.Length -gt 0 -and $seen.Add($candidate)) {
                $segments.Add($candidate)
            }
        }
    }

    return $segments -join ';'
}

# 목적: toolchain 값은 보존하면서 Windows 환경 key와 build temp를 정규화
# 입력: TempPath - sandbox에서도 쓸 수 있는 workspace 내부 temp directory
# 출력: child process에 전달할 case-insensitive environment dictionary
function New-CleanBuildEnvironment {
    param([string]$TempPath)

    $rawEnvironment = [Environment]::GetEnvironmentVariables([EnvironmentVariableTarget]::Process)
    $cleanEnvironment = [Collections.Generic.Dictionary[string, string]]::new(
        [StringComparer]::OrdinalIgnoreCase)

    foreach ($entry in ($rawEnvironment.GetEnumerator() | Sort-Object { [string]$_.Key })) {
        $name = [string]$entry.Key
        if ($name -inotmatch '^(path|temp|tmp)$') {
            $cleanEnvironment[$name] = [string]$entry.Value
        }
    }

    $cleanEnvironment['Path'] = Get-CanonicalPathValue -Environment $rawEnvironment
    $cleanEnvironment['TEMP'] = $TempPath
    $cleanEnvironment['TMP'] = $TempPath
    return $cleanEnvironment
}

# 목적: 정규화된 environment만 상속하는 동기 child process 실행
# 입력: FilePath, Arguments, Environment, WorkingDirectory - 실행 계약
# 출력: 성공 시 없음, process 시작 또는 종료 실패 시 terminating error
function Invoke-CleanProcess {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [Collections.Generic.Dictionary[string, string]]$Environment,
        [string]$WorkingDirectory
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.UseShellExecute = $false
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.Environment.Clear()

    foreach ($entry in $Environment.GetEnumerator()) {
        $startInfo.Environment[$entry.Key] = $entry.Value
    }
    foreach ($argument in $Arguments) {
        [void]$startInfo.ArgumentList.Add($argument)
    }

    Write-Host ('>> {0} {1}' -f $FilePath, ($Arguments -join ' '))
    $process = [Diagnostics.Process]::Start($startInfo)
    if ($null -eq $process) {
        throw "Failed to start $FilePath."
    }

    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "$FilePath exited with code $($process.ExitCode)."
    }
}

# 목적: PATH 또는 Visual Studio installation에서 bundled CMake executable 탐색
# 입력: 없음
# 출력: 사용할 cmake.exe absolute path, 탐색 실패 시 terminating error
function Resolve-CMakePath {
    $cmakeCommand = Get-Command cmake.exe -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -ne $cmakeCommand) {
        return $cmakeCommand.Source
    }

    $programFilesX86 = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)
    $vswherePath = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswherePath -PathType Leaf) {
        $vsRoot = [string](& $vswherePath -latest -products * `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
        $candidate = Join-Path $vsRoot.Trim() `
            'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    throw 'cmake.exe was not found on PATH or in the latest Visual Studio installation.'
}

if ($Fresh -and $SkipConfigure) {
    throw '-Fresh cannot be combined with -SkipConfigure.'
}
if ($Benchmarks -and $Configuration -ne 'Release') {
    throw '-Benchmarks currently supports the Release configuration only.'
}
if ($Benchmarks -and $Worker) {
    throw '-Benchmarks and -Worker select different build profiles and cannot be combined.'
}
if ($Mcp -and ($Benchmarks -or $Worker -or $Portable)) {
    throw '-Mcp cannot be combined with -Benchmarks, -Worker, or -Portable.'
}
if ($Mcp -and $Configuration -ne 'Release') {
    throw '-Mcp currently supports the Release configuration only.'
}
if ($Portable -and ($Benchmarks -or $Worker)) {
    throw '-Portable cannot be combined with -Benchmarks or -Worker.'
}
if ($Portable -and $Configuration -ne 'Release') {
    throw '-Portable currently supports the Release configuration only.'
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$tempPath = Join-Path $repoRoot 'build\.tmp\msbuild'
[void][IO.Directory]::CreateDirectory($tempPath)
$buildEnvironment = New-CleanBuildEnvironment -TempPath $tempPath

if ($EnvironmentCheckOnly) {
    $pathKeys = @($buildEnvironment.Keys | Where-Object { $_ -imatch '^path$' })
    Write-Output ('PATH_KEYS=' + ($pathKeys -join ','))
    Write-Output ('PATH_COUNT=' + $pathKeys.Count)
    Write-Output ('TEMP=' + $buildEnvironment['TEMP'])
    Write-Output ('TMP=' + $buildEnvironment['TMP'])

    if ($pathKeys.Count -ne 1 -or $pathKeys[0] -cne 'Path') {
        throw 'The normalized environment must contain exactly one canonical Path key.'
    }

    $probePath = Join-Path $tempPath ('flexraw-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::WriteAllText($probePath, 'ok')
    }
    finally {
        Remove-Item -LiteralPath $probePath -Force -ErrorAction SilentlyContinue
    }
    exit 0
}

$cmakePath = Resolve-CMakePath
$configurationKey = $Configuration.ToLowerInvariant()
$configurePreset = if ($Benchmarks) {
    'windows-msvc-release-benchmarks'
}
elseif ($Worker) {
    "windows-msvc-worker-$configurationKey"
}
elseif ($Mcp) {
    "windows-msvc-mcp-$configurationKey"
}
elseif ($Portable) {
    "windows-msvc-portable-contract-$configurationKey"
}
else {
    "windows-msvc-$configurationKey"
}
$buildPreset = if ($Benchmarks) {
    'benchmarks-release'
}
elseif ($Worker) {
    "worker-windows-$configurationKey"
}
elseif ($Mcp) {
    "mcp-windows-$configurationKey"
}
elseif ($Portable) {
    "portable-contract-windows-$configurationKey"
}
else {
    $configurationKey
}

if (-not $SkipConfigure) {
    $configureArguments = @()
    if ($Fresh) {
        $configureArguments += '--fresh'
    }
    $configureArguments += @('--preset', $configurePreset)
    Invoke-CleanProcess -FilePath $cmakePath -Arguments $configureArguments `
        -Environment $buildEnvironment -WorkingDirectory $repoRoot
}

$buildArguments = @(
    '--build', '--preset', $buildPreset,
    '--config', $Configuration,
    '--parallel', [string]$Jobs,
    '--', '/nr:false'
)
Invoke-CleanProcess -FilePath $cmakePath -Arguments $buildArguments `
    -Environment $buildEnvironment -WorkingDirectory $repoRoot

if ($Test) {
    $ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
    if (-not (Test-Path -LiteralPath $ctestPath -PathType Leaf)) {
        $ctestPath = (Get-Command ctest.exe -ErrorAction Stop).Source
    }

    Invoke-CleanProcess -FilePath $ctestPath `
        -Arguments @('--preset', $buildPreset, '--parallel', [string]$Jobs) `
        -Environment $buildEnvironment -WorkingDirectory $repoRoot
}

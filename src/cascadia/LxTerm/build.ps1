# Builds laymux_wt.dll out of this tree.
#
# The stock Windows Terminal build assumes a full Visual Studio install with the
# UWP C++ workload and Windows SDK 10.0.22621. LxTerm only needs TerminalCore as
# a plain desktop static library, so this script overrides the pieces of the
# repo's build configuration that would otherwise demand that workload.
#
# Usage:  pwsh src\cascadia\LxTerm\build.ps1 [-Configuration Release] [-Platform x64]

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64',

    [string]$VcpkgRoot = $env:VCPKG_ROOT,

    [string]$WindowsTargetPlatformVersion
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path

# --- toolchain discovery ---------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio 2022 (or Build Tools) with the C++ workload."
}
$vsRoot = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vsRoot) {
    throw "No Visual Studio installation with MSBuild was found."
}
$msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) {
    throw "MSBuild.exe not found under $vsRoot."
}

# Pick the newest installed Windows SDK unless the caller pinned one. The repo
# pins 10.0.22621, which is not necessarily what is on the machine.
if (-not $WindowsTargetPlatformVersion) {
    $sdkInclude = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
    $WindowsTargetPlatformVersion = Get-ChildItem $sdkInclude -Directory |
        Where-Object { $_.Name -match '^10\.' } |
        Sort-Object { [version]$_.Name } |
        Select-Object -Last 1 -ExpandProperty Name
    if (-not $WindowsTargetPlatformVersion) {
        throw "No Windows 10/11 SDK found under $sdkInclude."
    }
}

if (-not $VcpkgRoot) {
    throw "VCPKG_ROOT is not set. Clone microsoft/vcpkg (a FULL clone -- a shallow one cannot check out port trees) and bootstrap it."
}
if (-not (Test-Path (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
    throw "vcpkg.exe not found under $VcpkgRoot. Run bootstrap-vcpkg.bat there first."
}
$env:VCPKG_ROOT = $VcpkgRoot

# --- one-time NuGet restore ------------------------------------------------
# The repo drives packages.config restore through dep\nuget\packages.config.
# `msbuild /t:Restore` does not handle packages.config for C++ projects, and the
# bundled nuget.exe cannot parse OpenConsole.sln, so restore that file directly.
$wilTargets = Join-Path $repoRoot 'packages\Microsoft.Windows.ImplementationLibrary.1.0.240122.1\build\native\Microsoft.Windows.ImplementationLibrary.targets'
if (-not (Test-Path $wilTargets)) {
    Write-Host 'Restoring NuGet packages...' -ForegroundColor Cyan
    & (Join-Path $repoRoot 'dep\nuget\nuget.exe') restore (Join-Path $repoRoot 'dep\nuget\packages.config') `
        -PackagesDirectory (Join-Path $repoRoot 'packages') `
        -ConfigFile (Join-Path $repoRoot 'NuGet.Config') `
        -NonInteractive
    if ($LASTEXITCODE -ne 0) { throw "NuGet restore failed." }
}

# --- build -----------------------------------------------------------------
# TreatWarningAsError is hardcoded in src\common.build.pre.props, so it cannot be
# overridden with /p:. cl.exe honours the _CL_ environment variable by appending
# it after the command line, which lets /WX- win. Newer MSVC toolsets warn on
# code the pinned toolset accepted (e.g. C4706 in TerminalSelection.cpp), and we
# would rather not carry patches to upstream sources.
$env:_CL_ = '/WX-'

$args = @(
    (Join-Path $PSScriptRoot 'LxTerm.vcxproj')
    "/p:SolutionDir=$repoRoot\"
    "/p:Configuration=$Configuration"
    "/p:Platform=$Platform"
    # TerminalCore builds as a Windows Store static library by default, which
    # needs the UWP C++ workload. We only consume it from a desktop DLL.
    '/p:OpenConsoleUniversalApp=false'
    # Spectre-mitigated runtime libraries are a separate VS component.
    '/p:SpectreMitigation=false'
    "/p:WindowsTargetPlatformVersion=$WindowsTargetPlatformVersion"
    "/p:TargetPlatformVersion=$WindowsTargetPlatformVersion"
    '/nologo'
    '/v:m'
)

Write-Host "Building LxTerm ($Configuration|$Platform, SDK $WindowsTargetPlatformVersion)..." -ForegroundColor Cyan
& $msbuild @args
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

$out = Join-Path $repoRoot "bin\$Platform\$Configuration\LxTerm\laymux_wt.dll"
Write-Host "OK: $out" -ForegroundColor Green

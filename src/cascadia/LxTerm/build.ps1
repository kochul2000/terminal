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

# src\common.build.pre.props pins this SDK. If it is installed, leave the repo's
# own configuration alone; otherwise fall back to the newest one present.
$repoPinnedSdk = '10.0.22621.0'
$sdkInclude = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
$sdkPinnedInstalled = Test-Path (Join-Path $sdkInclude $repoPinnedSdk)
if (-not $WindowsTargetPlatformVersion) {
    $WindowsTargetPlatformVersion = Get-ChildItem $sdkInclude -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^10\.' } |
        Sort-Object { [version]$_.Name } |
        Select-Object -Last 1 -ExpandProperty Name
    if (-not $WindowsTargetPlatformVersion) {
        throw "No Windows 10/11 SDK found under $sdkInclude."
    }
} else {
    # An explicit request wins over the repo's pin.
    $sdkPinnedInstalled = $false
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
# it after the command line, which is the only lever left.
#
# Upstream CI runs Set-LatestVCToolsVersion.ps1, so it builds with whatever
# toolset the hosted image ships rather than a pinned one. Newer toolsets warn on
# code older ones accepted; as of MSVC 14.44 the only such warning in our
# dependency graph is C4706 (assignment in a conditional) in TerminalSelection.cpp.
# Suppress exactly that rather than /WX-, so a rebase onto a newer upstream tag
# still fails loudly on any new warning.
$env:_CL_ = '/wd4706'

$args = @(
    (Join-Path $PSScriptRoot 'LxTerm.vcxproj')
    "/p:SolutionDir=$repoRoot\"
    "/p:Configuration=$Configuration"
    "/p:Platform=$Platform"
    '/nologo'
    '/v:m'
)

# Every override below exists only because a VS component is missing. Probe for
# each one so that a fully provisioned machine builds exactly like upstream does.
if (-not $sdkPinnedInstalled) {
    $args += "/p:WindowsTargetPlatformVersion=$WindowsTargetPlatformVersion"
    $args += "/p:TargetPlatformVersion=$WindowsTargetPlatformVersion"
    Write-Host "  SDK $repoPinnedSdk not installed; targeting $WindowsTargetPlatformVersion instead." -ForegroundColor Yellow
}

# Spectre-mitigated runtime libraries ship as a separate VS component.
$spectreInstalled = [bool](Get-ChildItem (Join-Path $vsRoot 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName "lib\spectre\$Platform") })
if (-not $spectreInstalled) {
    $args += '/p:SpectreMitigation=false'
    Write-Host '  Spectre-mitigated libraries not installed; disabling SpectreMitigation.' -ForegroundColor Yellow
}

# TerminalCore builds as a Windows Store static library by default, which needs
# the UWP C++ workload. LxTerm only consumes it from a desktop DLL, so it can be
# built as a plain desktop library when that workload is absent.
$storeToolset = Join-Path $vsRoot 'MSBuild\Microsoft\VC\v170\Application Type\Windows Store\10.0'
if (-not (Test-Path $storeToolset)) {
    $args += '/p:OpenConsoleUniversalApp=false'
    Write-Host '  UWP C++ workload not installed; building TerminalCore as a desktop library.' -ForegroundColor Yellow
}

Write-Host "Building LxTerm ($Configuration|$Platform)..." -ForegroundColor Cyan
& $msbuild @args
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

$out = Join-Path $repoRoot "bin\$Platform\$Configuration\LxTerm\laymux_wt.dll"
Write-Host "OK: $out" -ForegroundColor Green

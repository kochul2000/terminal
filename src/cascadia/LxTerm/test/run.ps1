# Compiles and runs the LxTerm smoke test against a built laymux_wt.dll.
#
# Usage:  pwsh src\cascadia\LxTerm\test\run.ps1 [-Configuration Release] [-Platform x64]

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'ARM64')]
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'

$lxTermDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$repoRoot = (Resolve-Path (Join-Path $lxTermDir '..\..\..')).Path
$binDir = Join-Path $repoRoot "bin\$Platform\$Configuration\LxTerm"
$dll = Join-Path $binDir 'laymux_wt.dll'
$lib = Join-Path $binDir 'laymux_wt.lib'

if (-not (Test-Path $dll)) {
    throw "laymux_wt.dll not found at $dll. Run src\cascadia\LxTerm\build.ps1 first."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found under $vsRoot."
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) 'lxterm-smoke'
New-Item -ItemType Directory -Force $work | Out-Null
Copy-Item $dll $work -Force

# vcvars64.bat needs the Installer directory on PATH to find vswhere itself.
$installerDir = Split-Path $vswhere -Parent
$bat = Join-Path $work 'run.bat'
@"
@echo off
set "PATH=%PATH%;$installerDir"
call "$vcvars" >nul
cd /d "$work"
cl /nologo /W3 /I "$lxTermDir" "$(Join-Path $PSScriptRoot 'smoke.c')" /Fe:smoke.exe /link "$lib"
if errorlevel 1 exit /b 1
.\smoke.exe
"@ | Set-Content -Encoding ascii $bat

& cmd /c $bat
$exit = $LASTEXITCODE
if ($exit -ne 0) {
    throw "Smoke test failed (exit $exit)."
}
Write-Host 'Smoke test OK.' -ForegroundColor Green

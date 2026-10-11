#requires -Version 5.1
<#
.SYNOPSIS
    Build xLighter (based on Core UI).
.DESCRIPTION
    Sets MSVC + Windows SDK environment manually (no vcvars64.bat, which
    calls reg.exe), configures with cmake -G Ninja, and builds.
.PARAMETER Clean
    Remove build/ before configuring.
.PARAMETER Config
    Debug or Release (default Release).
#>
[CmdletBinding()]
param(
    [switch] $Clean,
    [ValidateSet('Debug','Release')]
    [string] $Config = 'Release'
)

$ErrorActionPreference = 'Stop'

$repo     = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repo 'build'

# ---------- toolchain paths (this machine) ----------
$vsRoot   = "${env:ProgramFiles}\Microsoft Visual Studio\18\Community"
$msvcVer  = '14.50.35717'
$sdkVer   = '10.0.26100.0'
$msvcDir  = Join-Path $vsRoot "VC\Tools\MSVC\$msvcVer"
$sdkDir   = 'D:\Windows Kits\10'

if (-not (Test-Path (Join-Path $msvcDir 'bin\Hostx64\x64\cl.exe'))) {
    throw "cl.exe not found under $msvcDir"
}
if (-not (Test-Path (Join-Path $sdkDir "Include\$sdkVer\um\Windows.h"))) {
    throw "Windows SDK $sdkVer not found under $sdkDir"
}

$ninja = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if (-not (Test-Path $ninja)) {
    $cmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($cmd) { $ninja = $cmd.Source }
}
if (-not (Test-Path $ninja)) { throw "ninja.exe not found." }

# ---------- build env ----------
$binDir  = Join-Path $msvcDir 'bin\Hostx64\x64'
$msvcInc = Join-Path $msvcDir 'include'
$msvcLib = Join-Path $msvcDir 'lib\x64'
$sdkInc  = Join-Path $sdkDir "Include\$sdkVer"
$sdkLib  = Join-Path $sdkDir "Lib\$sdkVer"

$env:INCLUDE = "$msvcInc;$sdkInc\ucrt;$sdkInc\shared;$sdkInc\um;$sdkInc\winrt;$sdkInc\cppwinrt"
$env:LIB     = "$msvcLib;$sdkLib\ucrt\x64;$sdkLib\um\x64"
$env:PATH    = "$binDir;$env:PATH"

Write-Host "MSVC  : $msvcDir"
Write-Host "SDK   : $sdkDir ($sdkVer)"
Write-Host "ninja : $ninja"
Write-Host ""

# ---------- clean ----------
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Cleaning $buildDir ..."
    Remove-Item -Recurse -Force $buildDir
}

# ---------- configure ----------
$needsConfigure = -not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))
if ($needsConfigure) {
    Write-Host "Configuring CMake ..."
    $ninjaFwd = $ninja -replace '\\','/'
    # 用 22621 的 rc.exe：26100 的 rc 在 ninja 子进程里会挂死（core-ui 文档踩过）。
    # （当前项目 rc 只编图标，经 PATH 找到的 rc 也能用；先显式放 PATH 兜底）
    $rcDir = 'D:\Windows Kits\10\bin\10.0.22621.0\x64'
    if (Test-Path "$rcDir\rc.exe") { $env:PATH = "$rcDir;$env:PATH" }
    & cmake -S $repo -B $buildDir -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$ninjaFwd" `
        "-DCMAKE_BUILD_TYPE=$Config" `
        "-DCMAKE_C_COMPILER=$binDir\cl.exe" `
        "-DCMAKE_CXX_COMPILER=$binDir\cl.exe"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
}

# ---------- build ----------
Write-Host "Building ..."
& cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

Write-Host ""
Write-Host "Done. Output in $buildDir"
Write-Host "  executable: $(Join-Path $buildDir 'xLighter.exe')"

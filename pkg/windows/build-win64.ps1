<#
.SYNOPSIS
  用 Visual Studio（MSBuild）編譯 Windows 64-bit 版 RetroArch。

.DESCRIPTION
  1. 用 vswhere 找到最新的 Visual Studio / MSBuild
  2. 編譯 pkg\msvc\RetroArch-msvc2022.sln（Release|x64）
  3. 把 exe 複製到輸出資料夾，改名為 retroarch.exe
  4. 下載官方 assets.zip 解壓到 assets\（選單圖示、字型；沒有它中文會變成 ????）

  直接編譯目前的工作目錄，包含還沒 commit 的修改。
  新的 .c 檔要記得加進 griffin\griffin.c，MSVC 只編 griffin。

.EXAMPLE
  pwsh pkg\windows\build-win64.ps1
  pwsh pkg\windows\build-win64.ps1 -Rebuild -OutDir D:\RetroArch-win64
#>
#Requires -Version 7
[CmdletBinding()]
param(
    # 輸出資料夾，預設是 repo 旁邊的 RetroArch-win64
    [string]$OutDir,
    # Release 或 Debug
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    # 全部重新編譯（預設是增量編譯）
    [switch]$Rebuild,
    # 重新下載 assets.zip
    [switch]$UpdateAssets,
    # 不處理 assets
    [switch]$SkipAssets
)

$ErrorActionPreference = 'Stop'

$RepoDir = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $OutDir) { $OutDir = Join-Path (Split-Path $RepoDir -Parent) 'RetroArch-win64' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

function Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

# ---------------------------------------------------------------- 1. MSBuild
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw '找不到 Visual Studio，請先安裝「使用 C++ 的桌面開發」' }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio 沒有安裝 C++ x64 工具組' }
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'

# 用這版 VS 自己的工具組。專案預設是 v143，在 VS2026 上連結會出現 LNK1103
$vsMajor = [int](& $vswhere -latest -products * -property installationVersion).Split('.')[0]
$toolset = switch ($vsMajor) { 18 { 'v145' } 17 { 'v143' } 16 { 'v142' } default { $null } }

# ---------------------------------------------------------------- 2. build
$commit = (git -C $RepoDir rev-parse --short HEAD).Trim()
Step "編譯 RetroArch $Configuration|x64（$commit，VS $vsMajor，$toolset）"

$msbuildArgs = @(
    (Join-Path $RepoDir 'pkg\msvc\RetroArch-msvc2022.sln')
    "/t:$(if ($Rebuild) { 'Rebuild' } else { 'Build' })"
    "/p:Configuration=$Configuration"
    '/p:Platform=x64'
    '/m', '/v:minimal', '/nologo'
)
if ($toolset) { $msbuildArgs += "/p:PlatformToolset=$toolset" }
& $msbuild @msbuildArgs
if ($LASTEXITCODE -ne 0) { throw 'MSBuild 編譯失敗' }

# ---------------------------------------------------------------- 3. copy
$exe = Join-Path $RepoDir "pkg\msvc\x64\$Configuration\RetroArch-msvc2022.exe"
Copy-Item $exe (Join-Path $OutDir 'retroarch.exe') -Force

# ---------------------------------------------------------------- 4. assets
if (-not $SkipAssets) {
    $assetsDir = Join-Path $OutDir 'assets'
    $assetsZip = Join-Path $OutDir 'assets.zip'
    if ($UpdateAssets -or -not (Test-Path (Join-Path $assetsDir 'pkg\chinese-fallback-font.ttf'))) {
        Step '下載 assets.zip（約 75 MB）'
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest 'https://buildbot.libretro.com/assets/frontend/assets.zip' -OutFile $assetsZip
        Step '解壓 assets'
        Expand-Archive $assetsZip -DestinationPath $assetsDir -Force
        Remove-Item $assetsZip
    } else {
        Write-Host '    assets 已存在，略過（要更新請加 -UpdateAssets）'
    }
}

# ---------------------------------------------------------------- 完成
$version = & (Join-Path $OutDir 'retroarch.exe') --version 2>&1 | Select-String 'Version'
Step "完成：$OutDir\retroarch.exe"
Write-Host "    $version"

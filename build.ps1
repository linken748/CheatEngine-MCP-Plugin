param(
    [switch]$Clean = $false
)

$ErrorActionPreference = "Stop"

$vsPath = "D:\Program Files\Microsoft Visual Studio\18\Community"
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"

if (-not (Test-Path $vcvars)) {
    Write-Error "vcvars64.bat not found at $vcvars"
    exit 1
}

$projRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$includeDir = Join-Path $projRoot "include"
$loaderSrc = Join-Path $projRoot "loader\ce_mcp_loader.cpp"
$coreSrc = Join-Path $projRoot "core\ce_mcp_core.cpp"
$serverSrc = Join-Path $projRoot "core\mcp_server.cpp"
$toolsSrc = Join-Path $projRoot "core\mcp_tools.cpp"
$binDir = Join-Path $projRoot "bin"

if ($Clean -and (Test-Path $binDir)) {
    Remove-Item $binDir -Recurse -Force
}

if (-not (Test-Path $binDir)) {
    New-Item -ItemType Directory -Path $binDir -Force | Out-Null
}

$deployDir = "E:\tools\Cheat Engine\plugins\ce_mcp"
if (-not (Test-Path $deployDir)) {
    New-Item -ItemType Directory -Path $deployDir -Force | Out-Null
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host " Building Cheat Engine MCP C++ Plugin   " -ForegroundColor Cyan
Write-Host " Toolchain: Visual Studio 2026 (MSVC)   " -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

# 编译 Loader DLL
Write-Host "[1/2] Compiling ce_mcp_plugin.dll (Loader)..." -ForegroundColor Yellow
$cmdLoader = @"
call "$vcvars"
cd /d "$binDir"
cl.exe /nologo /O2 /W3 /std:c++17 /EHsc /utf-8 /MD /I"$includeDir" "$loaderSrc" /LD /Fe:ce_mcp_plugin.dll /link /MACHINE:X64 /DEF:"$projRoot\loader\ce_mcp_loader.def" shlwapi.lib user32.lib
"@
$loaderBat = Join-Path $binDir "build_loader.bat"
Set-Content -Path $loaderBat -Value $cmdLoader -Encoding ASCII
& cmd.exe /c $loaderBat
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to build Loader DLL!"
    exit 1
}

# 编译 Core DLL
Write-Host "[2/2] Compiling ce_mcp_plugin_core.dll (Core + MCP Server)..." -ForegroundColor Yellow
$cmdCore = @"
call "$vcvars"
cd /d "$binDir"
cl.exe /nologo /O2 /W3 /std:c++17 /EHsc /utf-8 /MD /DWIN32_LEAN_AND_MEAN /D_WINSOCK_DEPRECATED_NO_WARNINGS /DCE_MCP_CORE_EXPORTS /I"$includeDir" "$coreSrc" "$serverSrc" "$toolsSrc" /LD /Fe:ce_mcp_plugin_core.dll /link /MACHINE:X64 ws2_32.lib psapi.lib user32.lib "$includeDir\lua53-64.lib"
"@
$coreBat = Join-Path $binDir "build_core.bat"
Set-Content -Path $coreBat -Value $cmdCore -Encoding ASCII
& cmd.exe /c $coreBat
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to build Core DLL!"
    exit 1
}

Write-Host "`n[Deployment] Copying DLLs to $deployDir..." -ForegroundColor Green
Copy-Item (Join-Path $binDir "ce_mcp_plugin.dll") $deployDir -Force
Copy-Item (Join-Path $binDir "ce_mcp_plugin_core.dll") $deployDir -Force

Write-Host "========================================" -ForegroundColor Green
Write-Host " Build & Deployment Succeeded!          " -ForegroundColor Green
Write-Host " Binaries located at:                   " -ForegroundColor Green
Write-Host " - $deployDir\ce_mcp_plugin.dll         " -ForegroundColor Green
Write-Host " - $deployDir\ce_mcp_plugin_core.dll    " -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green

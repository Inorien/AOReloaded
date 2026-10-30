# Builds the AOPlugins plugin (aoreloaded.dll, on Windows: tools\windows-build.ps1) and puts it into the client's
# AOPlugins folder. It only runs with AOPlugins' version.dll in the client folder; the standalone AOReloaded
# version.dll ignores the folder.
#
# Usage (from anywhere):  powershell -ExecutionPolicy Bypass -File tools\update-client.ps1 [-NoBuild] [-Client <dir>]
param(
    [string]$Client = "C:\Users\$env:USERNAME\DownloadsPRK\client",
    [switch]$NoBuild
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

function Fail([string]$message) { Write-Host "update-client: $message" -ForegroundColor Red; exit 1 }

if (-not (Test-Path (Join-Path $Client "AnarchyOnline.exe"))) { Fail "no AnarchyOnline.exe in $Client" }
if (Get-Process -Name "AnarchyOnline" -ErrorAction SilentlyContinue) { Fail "the game is running - close it first (its plugins are in use)" }

# 1. Build.
$plugin = Join-Path $repo "build\windows-release\aoreloaded.dll"
if (-not $NoBuild) {
    . (Join-Path $PSScriptRoot "windows-build.ps1")
    Write-Host "update-client: building (windows-release)"
    Invoke-WindowsBuild $repo -Target aoreloaded
    if ($LASTEXITCODE -ne 0) { Fail "the build failed" }
}
if (-not (Test-Path $plugin)) { Fail "no $plugin - build first" }

# 2. The plugin into the client's AOPlugins folder.
$plugins = Join-Path $Client "AOPlugins"
New-Item -ItemType Directory -Force $plugins | Out-Null
Copy-Item $plugin $plugins -Force
$hash = (Get-FileHash (Join-Path $plugins "aoreloaded.dll") -Algorithm SHA256).Hash.Substring(0, 12).ToLower()
Write-Host "update-client: aoreloaded.dll -> $plugins ($hash)"

# 3. Which version.dll the client has: AOPlugins loads the plugin; the standalone AOReloaded doesn't.
$version = Join-Path $Client "version.dll"
$text = if (Test-Path $version) { [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($version)) } else { "" }
if ($text.Contains("=== AOPlugins session")) {
    # nothing to say
} elseif ($text.Contains("AOReloaded")) {
    Write-Host "update-client: note - the client's version.dll is the standalone AOReloaded, which doesn't load plugins; replace it with AOPlugins' version.dll (AOPlugins: tools\update-client.ps1 there)" -ForegroundColor Yellow
} else {
    Write-Host "update-client: note - the client's version.dll isn't AOPlugins, so aoreloaded.dll isn't loaded (AOPlugins: tools\update-client.ps1 there)" -ForegroundColor Yellow
}
Write-Host "update-client: done"

# Packages the AOPlugins plugin (aoreloaded.dll) into a distributable folder/ZIP without modifying the game client.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\package-client.ps1
#   powershell -ExecutionPolicy Bypass -File tools\package-client.ps1 -NoBuild
#   powershell -ExecutionPolicy Bypass -File tools\package-client.ps1 -OutputDir dist
#
# Output:
#   <OutputDir>\aoreloaded-package\
#   <OutputDir>\aoreloaded-package.zip
#
# The package holds AOPlugins\aoreloaded.dll and an install.ps1 helper that puts it into the client's AOPlugins
# folder. AOPlugins itself (its version.dll) is not in the package.

param(
    [string]$OutputDir = "",
    [switch]$NoBuild,
    [switch]$NoZip
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

function Fail([string]$message) {
    Write-Host "package-client: $message" -ForegroundColor Red
    exit 1
}

if (-not $OutputDir) { $OutputDir = Join-Path $repo "dist" }
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
$packageDir = Join-Path $OutputDir "aoreloaded-package"
$plugin = Join-Path $repo "build\windows-release\aoreloaded.dll"

# 1. Build, unless requested not to.
if (-not $NoBuild) {
    . (Join-Path $PSScriptRoot "windows-build.ps1")
    Write-Host "package-client: building (windows-release)"
    Invoke-WindowsBuild $repo -Target aoreloaded
    if ($LASTEXITCODE -ne 0) { Fail "the build failed" }
}

if (-not (Test-Path -LiteralPath $plugin -PathType Leaf)) {
    Fail "no built plugin at $plugin (build first or omit -NoBuild)"
}

# 2. Recreate the package directory so removed files don't linger.
if (Test-Path -LiteralPath $packageDir) {
    Remove-Item -LiteralPath $packageDir -Recurse -Force
}
New-Item -ItemType Directory -Path (Join-Path $packageDir "AOPlugins") -Force | Out-Null
Copy-Item -LiteralPath $plugin -Destination (Join-Path $packageDir "AOPlugins\aoreloaded.dll")

# 3. Add a standalone installer. It does not build anything and does not alter files until run.
$installer = @'
param(
    [Parameter(Mandatory = $true)]
    [string]$Client
)
$ErrorActionPreference = "Stop"

function Fail([string]$message) {
    Write-Host "install: $message" -ForegroundColor Red
    exit 1
}

$sourcePlugin = Join-Path $PSScriptRoot "AOPlugins\aoreloaded.dll"
if (-not (Test-Path (Join-Path $Client "AnarchyOnline.exe"))) {
    Fail "no AnarchyOnline.exe in $Client"
}
if (Get-Process -Name "AnarchyOnline" -ErrorAction SilentlyContinue) {
    Fail "the game is running - close it first"
}
if (-not (Test-Path $sourcePlugin)) { Fail "package is missing AOPlugins\aoreloaded.dll" }

$plugins = Join-Path $Client "AOPlugins"
New-Item -ItemType Directory -Path $plugins -Force | Out-Null
Copy-Item $sourcePlugin $plugins -Force
$hash = (Get-FileHash (Join-Path $plugins "aoreloaded.dll") -Algorithm SHA256).Hash.Substring(0, 12).ToLower()
Write-Host "install: aoreloaded.dll installed to $plugins ($hash)"

# The plugin runs only with AOPlugins' version.dll in the client folder.
$version = Join-Path $Client "version.dll"
$text = if (Test-Path $version) { [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($version)) } else { "" }
if ($text.Contains("=== AOPlugins session")) {
    # nothing to say
} elseif ($text.Contains("AOReloaded")) {
    Write-Host "install: note - the client's version.dll is the standalone AOReloaded, which doesn't load plugins; replace it with AOPlugins' version.dll (your settings in AOReloaded.ini are taken over)" -ForegroundColor Yellow
} else {
    Write-Host "install: note - AOPlugins isn't installed (version.dll), so aoreloaded.dll isn't loaded yet" -ForegroundColor Yellow
}
Write-Host "install: done"
'@
Set-Content -LiteralPath (Join-Path $packageDir "install.ps1") -Value $installer -Encoding UTF8

# 4. Include a short readme and checksums for verifying the package.
$readme = @"
AOReloaded plugin package
=========================

Contents:
- AOPlugins\aoreloaded.dll: AOReloaded's features as a plugin for AOPlugins
- install.ps1: installer helper

Install (PowerShell):
  powershell -ExecutionPolicy Bypass -File .\install.ps1 -Client "C:\path\to\client"

Or copy AOPlugins\aoreloaded.dll into the client's AOPlugins folder yourself.

Close Anarchy Online before installing. The plugin needs AOPlugins (its version.dll in the
client folder): https://github.com/McAndersDK/aoplugins. It replaces the standalone
AOReloaded version.dll; your settings in AOReloaded.ini are taken over the first time.
Settings are in the AOReloaded tab of the options window (F10), saved to AOPlugins.ini.
Review install.ps1 before running it.
"@
Set-Content -LiteralPath (Join-Path $packageDir "README.txt") -Value $readme -Encoding UTF8

$hashLines = Get-ChildItem -LiteralPath $packageDir -File -Recurse |
    Where-Object { $_.Name -ne "SHA256SUMS.txt" } |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($packageDir.Length).TrimStart('\')
        "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $relative
    }
Set-Content -LiteralPath (Join-Path $packageDir "SHA256SUMS.txt") -Value $hashLines -Encoding UTF8

Write-Host "package-client: files staged in $packageDir"
if (-not $NoZip) {
    $zip = Join-Path $OutputDir "aoreloaded-package.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path (Join-Path $packageDir "*") -DestinationPath $zip -CompressionLevel Optimal
    Write-Host "package-client: ZIP created at $zip"
}
Write-Host "package-client: done"

# Runs aoreloaded.dll (the AOPlugins plugin build) in AOPlugins' fake client, next to ao-vk's plugin, and checks:
# both run (a project and a utility), the settings come from AOReloaded.ini the first time and are AOPlugins
# settings afterwards, the AOReloaded tab is served, the Skills.xml patch is in place, and the features that need the
# real game's DLLs (GUI.dll, N3.dll ...) refuse cleanly instead of crashing.
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\test-plugin-fake-client.ps1 [-AOPlugins <its build dir>]
#         [-AoVkPlugin <ao-vk.dll>] [-Keep]
param(
    [string]$AOPlugins = "C:\projekter\aorebirth-workspace\AOPlugins\build\windows-release",
    [string]$AoVkPlugin = "C:\projekter\aorebirth-workspace\ao-vk\build\windows-aoplugin\ao-vk.dll",
    [switch]$Keep
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $env:TEMP "aoreloaded-plugin-test"
$failures = 0

function Check([string]$what, [bool]$ok) {
    if ($ok) { Write-Host "  ok    $what" -ForegroundColor Green }
    else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:failures++ }
}
function Has([string]$text, [string]$literal) { return $text.Contains($literal) }

if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
$plugins = Join-Path $dir "AOPlugins"
New-Item -ItemType Directory -Force $plugins, (Join-Path $dir "cd_image\gui\Default\OptionPanel"), (Join-Path $dir "cd_image\gui\Default\Views") | Out-Null
Copy-Item (Join-Path $AOPlugins "version.dll") $dir
foreach ($f in "AnarchyOnline.exe", "Utils.dll", "AFCM.dll", "GUI.dll", "Awesomium.dll") { Copy-Item (Join-Path $AOPlugins "tests\$f") $dir }
Copy-Item (Join-Path $repo "build\windows-release\aoreloaded.dll") $plugins
if (Test-Path $AoVkPlugin) { Copy-Item $AoVkPlugin $plugins }
Set-Content (Join-Path $dir "cd_image\gui\Default\OptionPanel\Root.xml") -NoNewline -Value "<root>`n  <ScrollView label=`"General`"><View/></ScrollView>`n</root>`n"
Set-Content (Join-Path $dir "cd_image\gui\Default\Views\Skills.xml") -NoNewline -Value "<root/>`n"
Set-Content (Join-Path $dir "AOReloaded.ini") "[AOReloaded]`r`nAOR_CYawSpd=7`r`nAOR_TBarX=300`r`n"

$env:FAKE_CLIENT_SET = "AOR_CamOn=bool:0;AOR_TBarY=double:250"
$p = Start-Process -FilePath (Join-Path $dir "AnarchyOnline.exe") -WorkingDirectory $dir -PassThru -NoNewWindow `
    -RedirectStandardOutput (Join-Path $dir "stdout.txt")
$null = $p.Handle
if (-not $p.WaitForExit(30000)) { $p.Kill() }
Remove-Item Env:\FAKE_CLIENT_SET -ErrorAction SilentlyContinue
Check "the fake client exited cleanly (code $($p.ExitCode))" ($p.ExitCode -eq 0)

$log = Get-Content -Raw (Join-Path $dir "AOPlugins.log")
$ini = Get-Content -Raw (Join-Path $dir "AOPlugins.ini")
$xml = Get-Content -Raw (Join-Path $dir "served-Root.xml")
Check "aoreloaded runs" ($log -match "aoreloaded [0-9.]+: running")
if (Test-Path $AoVkPlugin) { Check "... next to ao-vk (a project)" ($log -match "ao-vk [0-9.]+: running") }
Check "AOReloaded.ini's values taken over" (($ini -match "AOR_CYawSpd=7") -and ($ini -match "AOR_TBarX=300"))
Check "settings changed in the window are saved" (($ini -match "AOR_CamOn=0") -and ($ini -match "AOR_TBarY=250"))
Check "the AOReloaded tab is served, with its settings" ((Has $xml 'label="AOReloaded"') -and (Has $xml 'opt_variable="AOR_CamOn"'))
Check "the moved settings aren't in it" (-not (Has $xml "AOR_FpsCap") -and -not (Has $xml "AOR_ObjDist"))
Check "the Skills.xml patch is registered (no crash in attach)" (Has $log "[init] AOReloaded ready")
Check "features without the game's DLLs refuse cleanly" (-not ($log -match "aoreloaded: crashed"))
$fakeExe = Join-Path $AOPlugins "tests\AnarchyOnline.exe"
$origExe = Join-Path $dir "AnarchyOnline_orig.exe"
Check "the LAA patch keeps the untouched exe as AnarchyOnline_orig.exe (for the launcher's check)" (
    (Test-Path $origExe) -and ((Get-FileHash $origExe).Hash -eq (Get-FileHash $fakeExe).Hash) -and
    ((Get-FileHash (Join-Path $dir "AnarchyOnline.exe")).Hash -ne (Get-FileHash $fakeExe).Hash))
Write-Host ""
Write-Host "aoreloaded's log lines:"
$log -split "`n" | Where-Object { $_ -match "\[aoreloaded\]" } | Select-Object -First 25 | ForEach-Object { "  $_" }

if (-not $Keep) { Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue }
if ($failures) { Write-Host "$failures check(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "all checks passed" -ForegroundColor Green

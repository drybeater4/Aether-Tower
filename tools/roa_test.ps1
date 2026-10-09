# Launch RoA (retrying when the mod loader fails to inject the bridge) and run the fake PT in the given mode.
#   roa_test.ps1 <char id> <mode> [extra arg]
param([string]$Char = 'stock:zetterburn', [string]$Mode = '', [Parameter(ValueFromRemainingArguments=$true)][string[]]$Extra = @())
$mods = 'G:\games\steamapps\common\Rivals of Aether\mods'
$log  = "$mods\pizzarivals.log"
$ok = $false
for ($try = 1; $try -le 4 -and -not $ok; $try++) {
    Stop-Process -Name RivalsofAether -Force -ErrorAction SilentlyContinue; Start-Sleep 2
    if (Test-Path $log) { $keep = "$mods\logs"; New-Item -ItemType Directory -Force $keep | Out-Null; Move-Item -LiteralPath $log "$keep\pizzarivals_$(Get-Date -Format yyyyMMdd_HHmmss).log" -Force }
    Start-Process -FilePath 'G:\games\steamapps\common\Rivals of Aether\RivalsofAether.exe' -WorkingDirectory 'G:\games\steamapps\common\Rivals of Aether'
    for ($i = 0; $i -lt 20; $i++) { Start-Sleep 3; if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'bridge: ready' -Quiet)) { $ok = $true; break } }
}
if (-not $ok) { 'RoA bridge did not load after 4 tries'; exit 1 }
$args2 = @($Char); if ($Mode) { $args2 += $Mode }; if ($Extra) { $args2 += $Extra }
Start-Process -FilePath "$PSScriptRoot\fake_pt.exe" -ArgumentList $args2 -RedirectStandardOutput C:\ptbuild\fake_pt.log -WindowStyle Hidden
for ($i = 0; $i -lt 90; $i++) { Start-Sleep 3; if (Select-String -Path C:\ptbuild\fake_pt.log -Pattern 'sequence done' -Quiet -ErrorAction SilentlyContinue) { break } }
Stop-Process -Name RivalsofAether, fake_pt -Force -ErrorAction SilentlyContinue
'done'

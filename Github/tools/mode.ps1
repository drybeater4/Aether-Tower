# Switch the RoA mod between the normal bridge and the capture probe.
#   mode.ps1 bridge   normal play (PizzaRivals bridge)
#   mode.ps1 probe    capture mode: start RoA yourself, pick a character in Training, play ~10 s; it writes
#                     mods\pizzarivals_dump_training.txt (every global/instance variable) and pizzarivals_hitbox.txt
# RoA must be closed. The two DLLs hook the same functions, so only one is installed at a time.
param([Parameter(Mandatory)][ValidateSet('bridge','probe')][string]$Mode)
if (Get-Process RivalsofAether -EA SilentlyContinue) { Write-Error 'Close Rivals of Aether first.'; exit 1 }
Set-Location "$PSScriptRoot\..\roa-bridge"
powershell -ExecutionPolicy Bypass -File build.ps1 -Deploy $Mode | Out-Null
$mods = 'G:\games\steamapps\common\Rivals of Aether\mods'
Remove-Item "$mods\pizzarivals.log", "$mods\pizzarivals_dump_*.txt" -EA SilentlyContinue
"Installed: $Mode"

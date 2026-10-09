# Starts Pizza Tower (patched build) and then Rivals of Aether (bridge mod). Usage: play.ps1 [-Room entrance_1]
#  - PT first, RoA ~20 s later (RoA waits for PT's request; its window moves off-screen once the match is ready)
#  - In PT: F6 = Rivals on/off, F7 = character menu
param([string]$Room = '')
$rt  = 'C:\ProgramData\GameMakerStudio2-LTS\Cache\runtimes\runtime-2022.0.3.99\Windows\x64\Runner.exe'
$out = 'C:\ptbuild\out'
$roa = 'G:\games\steamapps\common\Rivals of Aether'
if ($Room) { Set-Content "$out\pizzarivals_dev.txt" $Room -Encoding ascii } else { Remove-Item "$out\pizzarivals_dev.txt" -EA SilentlyContinue }
Start-Process $rt -ArgumentList '-game', "$out\PizzaTower_GM2.win" -WorkingDirectory $out
Start-Sleep 20
Start-Process "$roa\RivalsofAether.exe" -WorkingDirectory $roa

# Builds the x86 RoA-side DLLs.
#   build.ps1                 build both
#   build.ps1 -Deploy bridge  build, then install the bridge into RoA's mods folder (removing the probe)
#   build.ps1 -Deploy probe   same for the probe (the two hook the same functions: only ever deploy one)
param([ValidateSet('', 'bridge', 'probe')][string]$Deploy = '')
$env:PATH += ';C:\Program Files (x86)\Microsoft Visual Studio\Installer'
$vc  = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build'
$ugm = (Resolve-Path "$PSScriptRoot\..\roa-mod-loader\loader\UGMMS\include").Path
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force build | Out-Null
$common = "/nologo /LD /O2 /EHa /std:c++17 /MD /I`"$ugm`" /I`"$PSScriptRoot\..\protocol`" /Fo:build\ "
cmd /c "`"$vc\vcvars32.bat`" >nul && cl $common src\probe_main.cpp src\loader_api.cpp user32.lib /Fe:build\pizzarivals_probe.dll"
cmd /c "`"$vc\vcvars32.bat`" >nul && cl $common src\bridge_main.cpp src\loader_api.cpp user32.lib d3d11.lib dxgi.lib /Fe:build\pizzarivals.dll"
$mods = 'G:\games\steamapps\common\Rivals of Aether\mods'
if ($Deploy -eq 'bridge' -and (Test-Path build\pizzarivals.dll)) {
  Remove-Item "$mods\pizzarivals_probe.dll" -ErrorAction SilentlyContinue
  Copy-Item build\pizzarivals.dll $mods -Force
}
if ($Deploy -eq 'probe' -and (Test-Path build\pizzarivals_probe.dll)) {
  Remove-Item "$mods\pizzarivals.dll" -ErrorAction SilentlyContinue
  Copy-Item build\pizzarivals_probe.dll $mods -Force
  Copy-Item data\candidates.txt "$mods\pizzarivals_candidates.txt" -Force
}

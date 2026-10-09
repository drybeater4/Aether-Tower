# Builds layout_test.cpp for x86 (RoA side) and x64 (PT side) and runs both.
$env:PATH += ';C:\Program Files (x86)\Microsoft Visual Studio\Installer'
$vc = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build'
Set-Location $PSScriptRoot
foreach ($a in @(@('vcvars32.bat', 'x86'), @('vcvars64.bat', 'x64'))) {
  $exe = "lt_$($a[1]).exe"
  cmd /c "`"$vc\$($a[0])`" >nul && cl /nologo /EHsc /std:c++17 layout_test.cpp /Fe:$exe >nul && .\$exe"
}
Remove-Item *.obj, lt_*.exe -ErrorAction SilentlyContinue

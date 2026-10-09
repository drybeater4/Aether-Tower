# Builds pizzarivals_pt.dll (x64). Output goes next to the script and, if present, into the PT build dir.
$env:PATH += ';C:\Program Files (x86)\Microsoft Visual Studio\Installer'
$vc = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build'
Set-Location $PSScriptRoot
cmd /c "`"$vc\vcvars64.bat`" >nul && cl /nologo /LD /O2 /EHsc /std:c++17 pizzarivals_pt.cpp /Fe:pizzarivals_pt.dll"
Remove-Item *.obj, *.exp, *.lib -ErrorAction SilentlyContinue

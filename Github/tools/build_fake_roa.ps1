$env:PATH += ';C:\Program Files (x86)\Microsoft Visual Studio\Installer'
$vc = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build'
Set-Location $PSScriptRoot
cmd /c "`"$vc\vcvars64.bat`" >nul && cl /nologo /O2 /EHsc /std:c++17 fake_roa.cpp /Fe:fake_roa.exe && del fake_roa.obj"

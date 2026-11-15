# build_cod2mem.ps1 - build tools\cod2mem.exe (x86, MSVC)
$vc='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat'
cmd.exe /c "call `"$vc`" >nul && cd /d `"$PSScriptRoot`" && cl /nologo /MT /O2 /D_CRT_SECURE_NO_WARNINGS cod2mem.c /Fe:cod2mem.exe psapi.lib 2>&1"

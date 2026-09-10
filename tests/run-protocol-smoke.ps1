$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools are required.' }

$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$output = Join-Path $PSScriptRoot 'protocol-smoke.exe'
$testObject = Join-Path $PSScriptRoot 'protocol-smoke.obj'
$protocolObject = Join-Path $PSScriptRoot 'media-protocol.obj'
$source = Join-Path $PSScriptRoot 'protocol-smoke.cpp'
$protocol = Join-Path $PSScriptRoot '..\Project1\Project1\service\media-protocol.cpp'
$include = Join-Path $PSScriptRoot '..\Project1\Project1'

cmd.exe /d /s /c "`"$vcvars`" >nul && cl /nologo /std:c++17 /EHsc /I`"$include`" /c `"$source`" /Fo`"$testObject`" && cl /nologo /std:c++17 /EHsc /I`"$include`" /c `"$protocol`" /Fo`"$protocolObject`" && link /nologo `"$testObject`" `"$protocolObject`" crypt32.lib /OUT:`"$output`""
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $output
$exitCode = $LASTEXITCODE
Remove-Item -LiteralPath $output -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $testObject -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $protocolObject -Force -ErrorAction SilentlyContinue
exit $exitCode

param([ValidateSet('Release','Debug')][string]$Configuration='Release',[switch]$Build)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
Push-Location $taskRoot
try {
    if($Build) {
        cmake -S . -B build/desktop -A x64
        if($LASTEXITCODE){throw 'CMake configuration failed'}
        cmake --build build/desktop --config $Configuration --parallel 4
        if($LASTEXITCODE){throw 'Build failed'}
    }
    $taskGenerator=Join-Path $taskRoot "build/desktop/$Configuration/make-stream-media.exe"
    $taskPlayer=Join-Path $taskRoot "bin/desktop/$Configuration/native-player.exe"
    if(!(Test-Path -LiteralPath $taskGenerator) -or !(Test-Path -LiteralPath $taskPlayer)) {throw 'Build first, or run this script with -Build'}
    $taskFixtures=Join-Path $taskRoot 'build/demo-fixtures'
    if(!(Test-Path -LiteralPath (Join-Path $taskFixtures 'stream.flv'))) {
        & $taskGenerator $taskFixtures
        if($LASTEXITCODE){throw 'Fixture generation failed'}
    }
    & $taskPlayer --decoder ffmpeg --demo-fixtures $taskFixtures --data-root (Join-Path $taskRoot 'build/demo-data')
} finally {Pop-Location}

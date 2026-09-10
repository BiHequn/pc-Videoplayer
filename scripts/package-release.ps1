$ErrorActionPreference = 'Stop'

$repository = Split-Path $PSScriptRoot -Parent
$executable = Join-Path $repository 'bin\x64\Release\native-player.exe'
$distRoot = Join-Path $repository 'dist'
$packageRoot = Join-Path $distRoot 'VideoPlayer1'
$licenseRoot = Join-Path $packageRoot 'licenses'
$archive = Join-Path $distRoot 'VideoPlayer1-windows-x64.zip'

if (-not (Test-Path -LiteralPath $executable)) {
    throw "Release executable not found: $executable"
}

if (Test-Path -LiteralPath $packageRoot) {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}
if (Test-Path -LiteralPath $archive) {
    Remove-Item -LiteralPath $archive -Force
}

New-Item -ItemType Directory -Force -Path $licenseRoot | Out-Null
Copy-Item -LiteralPath $executable -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $repository 'README.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $repository 'THIRD_PARTY_NOTICES.md') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $repository 'vcpkg.json') -Destination $packageRoot
Copy-Item -LiteralPath (
    Join-Path $repository 'vcpkg_installed\x64-windows-static\share\ffmpeg\copyright'
) -Destination (Join-Path $licenseRoot 'ffmpeg.txt')
Copy-Item -LiteralPath (
    Join-Path $repository 'vcpkg_installed\x64-windows-static\share\nlohmann-json\copyright'
) -Destination (Join-Path $licenseRoot 'nlohmann-json.txt')

Compress-Archive -Path $packageRoot -DestinationPath $archive
Write-Output $archive

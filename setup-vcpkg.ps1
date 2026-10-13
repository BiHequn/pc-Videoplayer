$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$vcpkgRoot = Join-Path $repoRoot ".tools\vcpkg"
$baseline = "3d33b7f7a8121da6e4fa80b35618fc8e960be686"

if (-not (Test-Path -LiteralPath (Join-Path $vcpkgRoot ".git"))) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $vcpkgRoot) | Out-Null
    git clone https://github.com/microsoft/vcpkg.git $vcpkgRoot
}

git -C $vcpkgRoot fetch origin $baseline
git -C $vcpkgRoot checkout $baseline

& (Join-Path $vcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics

Write-Host "vcpkg is ready. Open Project1\Project1.sln and build x64."

Import-VisualStudioVars -Architecture amd64

$rootDir = Get-Location
$releasesDir = Join-Path $rootDir "releases"
$version = Get-Content "version.txt" -Raw

New-Item -ItemType Directory -Force -Path $releasesDir

function Build-And-Archive {
    param (
        [string]$triplet
    )
    cmake --build .
    $archiveName = "mtsum-v$version-$triplet"
    7z a -tzip -mx=9 "$releasesDir\$archiveName.zip" "mtsum.exe" *.dll
}

#cmake --preset release-ninja -DMTSUM_STATIC=OFF -DMTSUM_VCPKG=ON -B cmake-build-release-x64-windows
Set-Location "cmake-build-release-x64-windows"
Build-And-Archive -triplet "x64-windows"

Set-Location $rootDir

#cmake --preset release-ninja -DMTSUM_STATIC=ON -DMTSUM_VCPKG=ON -B cmake-build-release-x64-windows-static
Set-Location "cmake-build-release-x64-windows-static"
Build-And-Archive -triplet "x64-windows-static"

Set-Location $rootDir
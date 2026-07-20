param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$packageRoot = Join-Path $projectRoot "out/package/syncer-$Configuration"
$cmake = Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (-not $cmake) {
    $cmake = @(
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $cmake) { throw 'CMake 3.22 or newer was not found.' }
if (-not $env:ANDROID_NDK_HOME -and (Test-Path 'C:\android-ndk-r27d')) {
    $env:ANDROID_NDK_HOME = 'C:\android-ndk-r27d'
}
if (-not $env:ANDROID_NDK_HOME) { throw 'Set ANDROID_NDK_HOME to the Android NDK directory.' }
$cmakeBundleRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $cmake))
$ninja = Get-ChildItem $cmakeBundleRoot -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
if ($ninja) { $env:PATH = "$(Split-Path -Parent $ninja);$env:PATH" }

& $cmake --preset windows
if ($LASTEXITCODE -ne 0) { throw "Windows configuration failed with exit code $LASTEXITCODE." }
& $cmake --build --preset windows --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "Windows build failed with exit code $LASTEXITCODE." }
$boostSource = Join-Path $projectRoot 'out/build/windows/_deps/boost-src'
foreach ($abi in @('arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86')) {
    if (Test-Path $boostSource) {
        & $cmake --preset $abi "-DFETCHCONTENT_SOURCE_DIR_BOOST=$($boostSource -replace '\\', '/')"
    } else {
        & $cmake --preset $abi
    }
    if ($LASTEXITCODE -ne 0) { throw "$abi configuration failed with exit code $LASTEXITCODE." }
    & $cmake --build --preset $abi
    if ($LASTEXITCODE -ne 0) { throw "$abi build failed with exit code $LASTEXITCODE." }
}

if (Test-Path $packageRoot) {
    Get-ChildItem -LiteralPath $packageRoot -Force | Remove-Item -Recurse -Force
} else {
    New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null
}
Copy-Item (Join-Path $projectRoot "out/build/windows/$Configuration/syncer.exe") $packageRoot
foreach ($abi in @('arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86')) {
    $serverDirectory = Join-Path $packageRoot "servers/$abi"
    New-Item -ItemType Directory -Force -Path $serverDirectory | Out-Null
    Copy-Item (Join-Path $projectRoot "out/build/android-$abi/syncerd") $serverDirectory
}
Write-Host "Package created at $packageRoot"

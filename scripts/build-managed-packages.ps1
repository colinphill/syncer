param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$ServerRoot,
    [switch]$NoRestore
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $ServerRoot) {
    $ServerRoot = Join-Path $projectRoot "out/package/syncer-$Configuration/servers"
}
$required = @('arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86')
foreach ($abi in $required) {
    $server = Join-Path $ServerRoot "$abi/syncerd"
    if (-not (Test-Path $server -PathType Leaf)) { throw "Missing Android server: $server" }
}
$output = Join-Path $projectRoot 'out/package/nuget'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$serverProperty = (Resolve-Path $ServerRoot).Path
if (-not $NoRestore) {
    dotnet restore (Join-Path $projectRoot 'managed/Syncer.Managed.slnx')
    if ($LASTEXITCODE -ne 0) { throw "Managed solution restore failed with exit code $LASTEXITCODE." }
}
dotnet pack (Join-Path $projectRoot 'managed/src/Syncer.Client/Syncer.Client.csproj') `
    --configuration $Configuration --output $output --no-restore -p:SyncerServerRoot=$serverProperty
if ($LASTEXITCODE -ne 0) { throw "Syncer.Client pack failed with exit code $LASTEXITCODE." }
dotnet pack (Join-Path $projectRoot 'managed/src/Syncer.Tool/Syncer.Tool.csproj') `
    --configuration $Configuration --output $output --no-restore -p:SyncerServerRoot=$serverProperty
if ($LASTEXITCODE -ne 0) { throw "Syncer.Tool pack failed with exit code $LASTEXITCODE." }
Write-Host "Managed packages created at $output"

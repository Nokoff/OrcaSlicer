param(
    [string]$BuildDirectory = "build",
    [Parameter(Mandatory = $true)][string]$NsisDirectory,
    [string]$Configuration = "Release",
    [string]$PackageSuffix = "local-complete"
)

$ErrorActionPreference = "Stop"
$repository = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$build = (Resolve-Path $BuildDirectory).Path
$nsis = (Resolve-Path $NsisDirectory).Path
$config = Join-Path $build "CPackConfig.cmake"
if (-not (Test-Path -LiteralPath $config) -or -not (Test-Path -LiteralPath (Join-Path $nsis "makensis.exe"))) {
    throw "A configured CMake build and an NSIS compiler are required."
}
if ($PackageSuffix -notmatch '^[A-Za-z0-9._-]+$') {
    throw "PackageSuffix may contain only letters, digits, periods, underscores, and hyphens."
}

# Use a fresh payload so old binaries and user-generated files cannot enter setup.
$payload = Join-Path $build ("installer-stage-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $payload | Out-Null
& cmake --install $build --config $Configuration --prefix $payload
if ($LASTEXITCODE -ne 0) { throw "CMake installation into the payload failed." }

# CPack's default install rules read the source resources, which may contain the
# legacy web bundle. Stage the pinned CLI and its matching web bundle here instead.
& python -B (Join-Path $repository "scripts\packaging\stage_connect_bundle.py") `
    --lock (Join-Path $repository ".github\connect-bundle.lock.json") `
    --platform windows-x64 --resources-dir (Join-Path $payload "resources") `
    --cache-dir (Join-Path $build "connect-bundle-cache")
if ($LASTEXITCODE -ne 0) { throw "Connection runtime staging failed." }
$architecture = ""
$cache = Get-Content -LiteralPath (Join-Path $build "CMakeCache.txt")
if ($cache -match '^SLIC3R_MSVC_ARCH:STRING=AVX512$') { $architecture = "-avx512" }
elseif ($cache -match '^SLIC3R_MSVC_ARCH:STRING=AVX2$') { $architecture = "-avx2" }
$launcher = "snapmaker-orca$architecture"
$displayName = "Snapmaker Orca"
if ($architecture) { $displayName += " (" + $architecture.TrimStart('-').ToUpper().Replace('AVX512', 'AVX-512') + ")" }
foreach ($relative in @("$launcher.exe", "Snapmaker_Orca$architecture.dll", "resources\snapmaker_control_windows_x64.exe", "resources\web\myfiles\index.html")) {
    if (-not (Test-Path -LiteralPath (Join-Path $payload $relative))) {
        throw "The installer payload is missing $relative."
    }
}
$sourceCommit = (& git -C $repository rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw "Cannot identify the source revision." }
$sourceChanges = @(& git -C $repository status --porcelain)
@{
    sourceCommit = $sourceCommit
    sourceDirty = $sourceChanges.Count -ne 0
    packagedUtc = [DateTime]::UtcNow.ToString('o')
    architecture = $architecture.TrimStart('-')
    launcher = "$launcher.exe"
    appSha256 = (Get-FileHash -LiteralPath (Join-Path $payload "Snapmaker_Orca$architecture.dll")).Hash.ToLower()
    colourMatchPatch = $false
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $payload "local-build-info.json") -Encoding utf8

$output = Join-Path $build "installer"
New-Item -ItemType Directory -Path $output -Force | Out-Null
$packageConfig = Join-Path $build "CPackMergeFix.cmake"
$configPath = $config.Replace('\', '/')
$payloadPath = $payload.Replace('\', '/')
$outputPath = $output.Replace('\', '/')
@"
include([==[$configPath]==])
set(CPACK_INSTALL_CMAKE_PROJECTS "")
set(CPACK_INSTALLED_DIRECTORIES [==[$payloadPath]==] "/")
set(CPACK_PACKAGE_DIRECTORY [==[$outputPath]==])
set(CPACK_PACKAGE_EXECUTABLES "$launcher;$displayName")
set(CPACK_NSIS_DISPLAY_NAME "$displayName")
set(CPACK_NSIS_PACKAGE_NAME "$displayName")
string(APPEND CPACK_PACKAGE_FILE_NAME "-$PackageSuffix")
"@ | Set-Content -LiteralPath $packageConfig -Encoding utf8

$previousPath = $env:PATH
try {
    $env:PATH = $nsis + ";" + $env:PATH
    & cpack --config $packageConfig -C $Configuration -G NSIS -B $output
    if ($LASTEXITCODE -ne 0) { throw "NSIS installer generation failed." }
} finally {
    $env:PATH = $previousPath
}
Write-Output "Installer output: $output"
Write-Output "Installer payload: $payload"

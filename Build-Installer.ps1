# Builds dist\VR-Gaze-Mirror-<version>.msi: the gaze mirror layer, the SteamVR helper, the OBS plugin, the mirror
# window and the settings app (all from v2\), plus the official Quad-Views-Foveated installer as an optional extra.
# Prerequisites: Visual Studio 2022 (C++), the .NET 10 SDK, the GitHub CLI for v2\Get-External.ps1 (first time only),
# and internet access the first time (the WiX toolset comes from nuget.org as part of the build).
#
#   .\Build-Installer.ps1 -Version 2.1.0                 a normal build
#   .\Build-Installer.ps1 -Version 2.1.0 -Label beta.1   a beta: the app calls itself 2.1.0-beta.1, the MSI is 2.1.0
#
# Every published build - beta or not - needs its own x.y.z: Windows Installer only upgrades to a higher number, and the
# app's update check compares those numbers. Whether a release counts as a beta is decided by GitHub's "pre-release" tick.
param(
    [string]$Version = '2.1.0',
    [string]$Label = '',
    # owner/name of the repository whose releases the app's update check looks at. '' = build without update checks.
    [string]$GitHubRepository = 'KanaiCasual/VR-Gaze-Mirror'
)

$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "-Version must be x.y.z (got '$Version'). Put 'beta.1' etc. in -Label." }
$root = $PSScriptRoot
$dist = Join-Path $root 'dist'
$appOut = Join-Path $dist 'app'
$appVersion = if ($Label) { "$Version-$Label" } else { $Version }

# 1) Third-party files (fetched once) and every 2.0 binary, with the offline layer test.
& (Join-Path $root 'v2\Get-External.ps1') | Out-Null
& (Join-Path $root 'v2\Build-Dev.ps1') | Out-Null

# 2) Layer, OBS plugin, Quad-Views-Foveated installer and licences -> GazeOverlayApp\Payload
& (Join-Path $root 'GazeOverlayApp\Collect-Payload.ps1') | Out-Null

# 3) Settings app, self-contained, as a plain folder (the MSI installs it).
if (Test-Path $appOut) { Remove-Item $appOut -Recurse -Force }
dotnet publish (Join-Path $root 'GazeOverlayApp\GazeOverlayApp.csproj') -c Release -o $appOut -v q --nologo `
    "-p:Version=$appVersion" "-p:GitHubRepository=$GitHubRepository"
if ($LASTEXITCODE -ne 0) { throw "Publishing the settings app failed." }

# 3b) The mirror window and the SteamVR helper, next to the settings app (the app finds them there).
Copy-Item (Join-Path $root 'v2\bin\mirror-window\MirrorWindow.exe') $appOut -Force
Copy-Item (Join-Path $root 'v2\bin\helper\GazeMirrorHelper.exe') $appOut -Force
Copy-Item (Join-Path $root 'v2\bin\helper\openvr_api.dll') $appOut -Force
# The helper's SteamVR manifest (relative binary path): shipped next to the exe, so an uninstall takes it away too.
Copy-Item (Join-Path $root 'v2\openvr-helper\GazeMirrorHelper.vrmanifest') $appOut -Force

# 4) The MSI.
$payload = Join-Path $root 'GazeOverlayApp\Payload'
dotnet build (Join-Path $root 'Installer\GazeOverlay.Installer.wixproj') -c Release -v q --nologo `
    "-p:ProductVersion=$Version" "-p:PayloadDir=$payload" "-p:AppDir=$appOut"
if ($LASTEXITCODE -ne 0) { throw "Building the MSI failed." }

$msi = Get-ChildItem (Join-Path $root 'Installer\bin') -Recurse -Filter 'VR-Gaze-Mirror.msi' |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
$target = Join-Path $dist "VR-Gaze-Mirror-$appVersion.msi"
Copy-Item $msi.FullName $target -Force
# The checksum file the app's updater checks a download against. Upload it with the .msi to every release
# (gh release create vX.Y.Z dist\<msi> dist\<msi>.sha256 ...); without it the app only links to the release page.
$hash = (Get-FileHash $target -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$target.sha256", "$hash  $(Split-Path $target -Leaf)`n")
'{0}  ({1:N1} MB)  SHA-256 {2}' -f $target, ((Get-Item $target).Length / 1MB), $hash

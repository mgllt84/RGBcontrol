$ErrorActionPreference = 'Stop'

$projectRoot = (Resolve-Path $PSScriptRoot).Path
$workspaceRoot = (Resolve-Path "$PSScriptRoot\..").Path
$toolRoot = (Resolve-Path "$workspaceRoot\work\w64devkit\w64devkit").Path
$toolBin = Join-Path $toolRoot 'bin'
$compiler = Join-Path $toolBin 'g++.exe'
$resourceCompiler = Join-Path $toolBin 'windres.exe'
$installerCompiler = Join-Path $toolBin 'makensis.exe'
$csharpCompiler = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$dotnetSdk = Join-Path $workspaceRoot 'work\dotnet-sdk-10.0.401\dotnet.exe'
$hidMaestroCore = Join-Path $workspaceRoot 'work\HIDMaestro-v1.7.3\HIDMaestro.Core.dll'
$env:PATH = "$toolBin;$env:PATH"

foreach ($required in @($compiler, $resourceCompiler, $installerCompiler, $csharpCompiler, $dotnetSdk, $hidMaestroCore)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Outil manquant : $required" }
}

$buildDirectory = Join-Path $projectRoot 'build'
$distDirectory = Join-Path $projectRoot 'dist'
$outputDirectory = Join-Path $workspaceRoot 'outputs\RGBCcontrol-CPP'
$legacyDirectory = Join-Path $workspaceRoot 'outputs\RGBCcontrol'
$appVersion = '0.16.22'
New-Item -ItemType Directory -Force -Path $buildDirectory,$distDirectory,$outputDirectory | Out-Null

& (Join-Path $projectRoot 'tools\MakeIcon.ps1') | Out-Null

$resourceObject = Join-Path $buildDirectory 'app-resources.o'
& $resourceCompiler -i (Join-Path $projectRoot 'resources\app.rc') -o $resourceObject -O coff
if ($LASTEXITCODE -ne 0) { throw 'La compilation des ressources a échoué.' }

$executable = Join-Path $buildDirectory 'RGBCcontrol.exe'
$cppArguments = @(
    '-std=c++20','-O2','-Wall','-Wextra','-municode','-mwindows','-static','-static-libgcc','-static-libstdc++',
    (Join-Path $projectRoot 'src\main.cpp'),(Join-Path $projectRoot 'src\openrgb.cpp'),(Join-Path $projectRoot 'src\plugin_engine.cpp'),(Join-Path $projectRoot 'src\audio_loopback.cpp'),
    (Join-Path $projectRoot 'src\screen_capture.cpp'),$resourceObject,
    '-o',$executable,'-lgdiplus','-ldwmapi','-lsetupapi','-lcrypt32','-lws2_32','-lshell32','-lcomdlg32','-lole32','-luuid','-lurlmon','-ladvapi32'
)
& $compiler @cppArguments
if ($LASTEXITCODE -ne 0) { throw 'La compilation C++ a échoué.' }

# Validate the external-provider contract with a harmless process plugin.  The
# sample never opens hardware; it checks manifest hashing, command dispatch,
# response framing and clean stop on every release build.
$pluginTestDirectory = Join-Path $buildDirectory 'plugin-engine-test'
New-Item -ItemType Directory -Force -Path $pluginTestDirectory | Out-Null
$samplePlugin = Join-Path $pluginTestDirectory 'sample-plugin.exe'
$pluginSmoke = Join-Path $pluginTestDirectory 'plugin-engine-smoke.exe'
& $compiler -std=c++20 -O2 -Wall -Wextra -municode -static -static-libgcc -static-libstdc++ `
    (Join-Path $projectRoot 'tools\sample_plugin.cpp') -o $samplePlugin
if ($LASTEXITCODE -ne 0) { throw 'La compilation du plugin de test a échoué.' }
& $compiler -std=c++20 -O2 -Wall -Wextra -municode -static -static-libgcc -static-libstdc++ `
    (Join-Path $projectRoot 'tools\plugin_engine_smoke.cpp') (Join-Path $projectRoot 'src\plugin_engine.cpp') `
    -o $pluginSmoke -lsetupapi -lcrypt32
if ($LASTEXITCODE -ne 0) { throw 'La compilation du test du moteur de plugins a échoué.' }
$sampleHash = (Get-FileHash -LiteralPath $samplePlugin -Algorithm SHA256).Hash.ToLowerInvariant()
$pluginTestManifest = Join-Path $pluginTestDirectory 'protocol-test.rgbcplugin'
Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\plugin_test_manifest.rgbcplugin.template') -Destination $pluginTestManifest -Force
$pluginManifestText = (Get-Content -LiteralPath $pluginTestManifest -Raw).Replace('__SAMPLE_SHA256__', $sampleHash)
Set-Content -LiteralPath $pluginTestManifest -Value $pluginManifestText -Encoding Unicode
& $pluginSmoke $pluginTestDirectory
if ($LASTEXITCODE -ne 0) { throw "Le test du moteur de plugins a échoué (code $LASTEXITCODE)." }

# Official isolated driver for the Sony DualSense RGB lightbar and player
# indicators. A HID failure remains outside the main RGBCcontrol process.
$dualsenseBuildDirectory = Join-Path $buildDirectory 'dualsense-plugin'
New-Item -ItemType Directory -Force -Path $dualsenseBuildDirectory | Out-Null
$dualsenseDriver = Join-Path $dualsenseBuildDirectory 'RGBCcontrol.DualSense.exe'
& $compiler -std=c++20 -O2 -Wall -Wextra -municode -static -static-libgcc -static-libstdc++ `
    (Join-Path $projectRoot 'tools\dualsense_plugin.cpp') -o $dualsenseDriver -lsetupapi -lhid
if ($LASTEXITCODE -ne 0) { throw 'La compilation du pilote DualSense a échoué.' }
& $dualsenseDriver --self-test
if ($LASTEXITCODE -ne 0) { throw "Le test du pilote DualSense a échoué (code $LASTEXITCODE)." }

# HIDMaestro provides a modern user-mode XInput bridge (no ViGEmBus kernel
# driver). Publish it self-contained so a colleague can use Xbox mode without
# installing .NET separately. The SDK and Core DLL are kept in work/ and are
# verified by the preparation step before a release build.
$gamepadBridgeBuild = Join-Path $buildDirectory 'gamepad-bridge'
if (Test-Path -LiteralPath $gamepadBridgeBuild) { Remove-Item -LiteralPath $gamepadBridgeBuild -Recurse -Force }
$gamepadBridgeProject = Join-Path $projectRoot 'src\gamepad_bridge\GamepadBridge.csproj'
& $dotnetSdk publish $gamepadBridgeProject -c Release -r win-x64 --self-contained true --no-restore -o $gamepadBridgeBuild
if ($LASTEXITCODE -ne 0) { throw 'La compilation du pont XInput a échoué.' }
$gamepadBridgeExecutable = Join-Path $gamepadBridgeBuild 'RGBCcontrol.GamepadBridge.exe'
& $gamepadBridgeExecutable --self-test
if ($LASTEXITCODE -ne 0) { throw "Le test du pont XInput a échoué (code $LASTEXITCODE)." }

$hardwareSource = Join-Path $legacyDirectory 'HardwareMonitor'
$hardwareBuild = Join-Path $buildDirectory 'HardwareMonitor'
New-Item -ItemType Directory -Force -Path $hardwareBuild | Out-Null
Copy-Item -Path (Join-Path $hardwareSource '*') -Destination $hardwareBuild -Recurse -Force
$bridge = Join-Path $hardwareBuild 'RGBCcontrol.HardwareBridge.exe'
& $csharpCompiler /nologo /target:exe /platform:x64 /optimize+ "/out:$bridge" "/reference:$hardwareBuild\LibreHardwareMonitorLib.dll" (Join-Path $projectRoot 'src\hardware_bridge.cs')
if ($LASTEXITCODE -ne 0) { throw 'La compilation du pont matériel a échoué.' }
Copy-Item -LiteralPath (Join-Path $projectRoot 'resources\RGBCcontrol.HardwareBridge.exe.config') -Destination "$bridge.config" -Force

if (-not $distDirectory.StartsWith($projectRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Dossier dist non sûr.' }
if (Test-Path -LiteralPath $distDirectory) {
    Get-ChildItem -LiteralPath $distDirectory -Force | Remove-Item -Recurse -Force
}
Copy-Item -LiteralPath $executable -Destination (Join-Path $distDirectory 'RGBCcontrol.exe') -Force
Copy-Item -LiteralPath (Join-Path $legacyDirectory 'assets') -Destination (Join-Path $distDirectory 'assets') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'resources\dualsense-controller.png') -Destination (Join-Path $distDirectory 'assets\dualsense-controller.png') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'resources\dualsense.dae') -Destination (Join-Path $distDirectory 'assets\dualsense.dae') -Force
New-Item -ItemType Directory -Force -Path (Join-Path $distDirectory 'assets\textures') | Out-Null
Copy-Item -Path (Join-Path $projectRoot 'resources\textures\*') -Destination (Join-Path $distDirectory 'assets\textures') -Force
Copy-Item -LiteralPath (Join-Path $legacyDirectory 'OpenRGB') -Destination (Join-Path $distDirectory 'OpenRGB') -Recurse -Force
Copy-Item -LiteralPath $hardwareBuild -Destination (Join-Path $distDirectory 'HardwareMonitor') -Recurse -Force
Copy-Item -LiteralPath $gamepadBridgeBuild -Destination (Join-Path $distDirectory 'GamepadBridge') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'LISEZ-MOI.txt') -Destination (Join-Path $distDirectory 'LISEZ-MOI.txt') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'resources\RGBCcontrol.ico') -Destination (Join-Path $distDirectory 'RGBCcontrol.ico') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'resources\update-channel.ini') -Destination (Join-Path $distDirectory 'update-channel.ini') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'licenses') -Destination (Join-Path $distDirectory 'Licences') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'work\HIDMaestro-v1.7.3\LICENSE') -Destination (Join-Path $distDirectory 'Licences\HIDMaestro-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'work\HIDMaestro-v1.7.3\THIRD-PARTY-NOTICES.txt') -Destination (Join-Path $distDirectory 'Licences\HIDMaestro-THIRD-PARTY-NOTICES.txt') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'plugins') -Destination (Join-Path $distDirectory 'Plugins') -Recurse -Force
$dualsensePluginDirectory = Join-Path $distDirectory 'Plugins\Sony-DualSense'
New-Item -ItemType Directory -Force -Path $dualsensePluginDirectory | Out-Null
Copy-Item -LiteralPath $dualsenseDriver -Destination (Join-Path $dualsensePluginDirectory 'RGBCcontrol.DualSense.exe') -Force
$dualsenseHash = (Get-FileHash -LiteralPath $dualsenseDriver -Algorithm SHA256).Hash.ToLowerInvariant()
foreach ($dualsenseDefinition in @(
    @{ File = 'sony-dualsense.rgbcplugin'; Id = 'com.sony.dualsense'; Name = 'Sony DualSense'; Pid = '0x0CE6'; Device = 'DualSense Wireless Controller' },
    @{ File = 'sony-dualsense-edge.rgbcplugin'; Id = 'com.sony.dualsense-edge'; Name = 'Sony DualSense Edge'; Pid = '0x0DF2'; Device = 'DualSense Edge Wireless Controller' }
)) {
    $manifestLines = @(
        '[RGBCPlugin]',
        'Api=2',
        'Enabled=1',
        "Id=$($dualsenseDefinition.Id)",
        "Name=$($dualsenseDefinition.Name) lighting driver",
        'Publisher=RGBCcontrol',
        'Transport=hid-usb-bluetooth',
        'Executable=RGBCcontrol.DualSense.exe',
        "Sha256=$dualsenseHash",
        'Vid=0x054C',
        "Pid=$($dualsenseDefinition.Pid)",
        "DeviceName=$($dualsenseDefinition.Device)",
        'DeviceType=Manette',
        'Capabilities=lighting,effects,hotplug,zones',
        'Modes=Static',
        'Zones=2',
        'Leds=7',
        'TimeoutMs=3000',
        "SupersedesOpenRGB=$($dualsenseDefinition.Device);DualSense"
    )
    Set-Content -LiteralPath (Join-Path $dualsensePluginDirectory $dualsenseDefinition.File) -Value $manifestLines -Encoding Unicode
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'PLUGIN-SDK.md') -Destination (Join-Path $distDirectory 'PLUGIN-SDK.md') -Force

$applicationOutput = Join-Path $outputDirectory 'Application'
if (-not $applicationOutput.StartsWith($outputDirectory, [StringComparison]::OrdinalIgnoreCase)) { throw 'Dossier de sortie non sûr.' }
if (Test-Path -LiteralPath $applicationOutput) { Remove-Item -LiteralPath $applicationOutput -Recurse -Force }
Copy-Item -LiteralPath $distDirectory -Destination $applicationOutput -Recurse -Force

Push-Location (Join-Path $projectRoot 'installer')
try {
    & $installerCompiler /V2 'RGBCcontrol.nsi'
    if ($LASTEXITCODE -ne 0) { throw "La création de l’installateur a échoué." }
} finally { Pop-Location }

$shareDirectory = Join-Path $outputDirectory 'Package-a-partager'
if (Test-Path -LiteralPath $shareDirectory) { Remove-Item -LiteralPath $shareDirectory -Recurse -Force }
New-Item -ItemType Directory -Path $shareDirectory | Out-Null
$versionedInstallerName = "RGBCcontrol-Setup-$appVersion.exe"
$versionedInstaller = Join-Path $shareDirectory $versionedInstallerName
Copy-Item -LiteralPath (Join-Path $outputDirectory 'RGBCcontrol-Setup.exe') -Destination $versionedInstaller -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'LISEZ-MOI.txt') -Destination (Join-Path $shareDirectory 'LISEZ-MOI.txt') -Force
$installerHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $versionedInstaller).Hash
Set-Content -LiteralPath (Join-Path $shareDirectory 'SHA256.txt') -Encoding ascii -Value "$installerHash  $versionedInstallerName"
$updateManifestName = 'RGBCcontrol-update.ini'
$releaseAssetUrl = "https://github.com/mgllt84/RGBcontrol/releases/download/v$appVersion/$versionedInstallerName"
$updateManifestLines = @(
    '[RGBCcontrol]',
    "version=$appVersion",
    "installer_url=$releaseAssetUrl",
    "sha256=$($installerHash.ToLowerInvariant())"
)
Set-Content -LiteralPath (Join-Path $shareDirectory $updateManifestName) -Encoding ascii -Value $updateManifestLines
$publicationInstructions = @(
    "Publication de RGBCcontrol $appVersion sur GitHub",
    '',
    "1. Créer une Release avec le tag v$appVersion dans https://github.com/mgllt84/RGBcontrol/releases/new",
    "2. Ajouter $versionedInstallerName et $updateManifestName comme fichiers de la Release",
    '3. Publier la Release. Le bouton de mise à jour la détectera automatiquement.'
)
Set-Content -LiteralPath (Join-Path $shareDirectory 'PUBLIER-SUR-GITHUB.txt') -Encoding utf8 -Value $publicationInstructions
$shareArchive = Join-Path $outputDirectory "RGBCcontrol-$appVersion-a-partager.zip"
if (Test-Path -LiteralPath $shareArchive) { Remove-Item -LiteralPath $shareArchive -Force }
Compress-Archive -Path (Join-Path $shareDirectory '*') -DestinationPath $shareArchive -CompressionLevel Optimal

Write-Output "Application : $outputDirectory\Application\RGBCcontrol.exe"
Write-Output "Installateur : $outputDirectory\RGBCcontrol-Setup.exe"
Write-Output "Package à partager : $shareArchive"
